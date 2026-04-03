#include <WiFi.h>
#include <DNSServer.h>
#include <Adafruit_BMP085.h>

#include "config.h"
#include "flash.h"
#include "lcd_display.h"
#include "web_handlers.h"

// ===== Sensor data (shared with flash.h and web_handlers.h via extern) =====
float temp = 0;
float pressureHPa = 0;
float pressureMmHg = 0;
float altitude = 0;

// ===== WiFi credentials — loaded from flash; configured via captive portal =====
char ssid[33] = {};
char password[65] = {};

// ===== Runtime state =====
Adafruit_BMP085 bmp;
DNSServer dnsServer;
bool apMode = false;
bool ledState = false;

uint32_t lastSaveMs = 0;
uint32_t lastLcdMs = 0;

// Wi-Fi reconnect watchdog
uint32_t wifiRetryMs = 0;
uint16_t wifiRetryCount = 0;
bool wifiWasLost = false;

// =============================================================================

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_BTN, INPUT_PULLUP);
  pinMode(WRITE_LED, OUTPUT);
  digitalWrite(WRITE_LED, LOW);

  flashInit();

  if (flashOK && readCredsFromFlash()) {
    Serial.printf("Loaded credentials from flash: SSID=%s\n", ssid);
  } else {
    Serial.println("No credentials in flash — will start captive portal AP");
  }

  // Initialize LCDs
  lcd.init();
  lcd.backlight();
  lcd2.init();
  lcd2.backlight();

  // Splash
  lcd.setCursor(3, 1);
  lcd.print("BMP180 Station");
  delay(1500);
  lcd.clear();

  // BMP180 init
  lcd.setCursor(0, 0);
  lcd.print("[1/2] Sensor...");
  Serial.println("Initializing BMP180...");
  if (!bmp.begin()) {
    lcd.setCursor(0, 1);
    lcd.print("FAIL: BMP180 missing");
    Serial.println("BMP180 not found!");
    while (true) { delay(1000); }
  }
  lcd.setCursor(0, 1);
  lcd.print("OK: BMP180 ready");
  Serial.println("BMP180 OK");
  delay(1000);

  // WiFi connect
  lcd.setCursor(0, 2);
  lcd.print("[2/2] WiFi...");
  int attempts = 0, lastStatus = -1;
  if (ssid[0] != '\0') {
    Serial.printf("Connecting to: %s\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(500);
    WiFi.begin(ssid, password);
  } else {
    Serial.println("No SSID — skipping connect, going to AP mode");
  }
  while (ssid[0] != '\0' && WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(1000);
    attempts++;
    int status = WiFi.status();
    if (status != lastStatus || attempts % 5 == 0) {
      lcd.setCursor(0, 3);
      lcd.print("                    ");
      lcd.setCursor(0, 3);
      lcd.print(wifiStatusToString(status));
      lcd.print(" (");
      lcd.print(attempts);
      lcd.print("/15)");
      lastStatus = status;
    }
    Serial.printf("  Attempt %d/15 - Status: %s (%d)\n", attempts, wifiStatusToString(status), status);
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }
  lcd.clear();

  if (WiFi.status() != WL_CONNECTED) {
    // Start captive portal AP
    Serial.println("WiFi FAILED — starting AP captive portal");
    apMode = true;
    IPAddress apIP(192, 168, 0, 1);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("TempWatcher");
    dnsServer.start(53, "*", apIP);
    server.on("/", handleProvision);
    server.on("/api/scan", handleScan);
    server.on("/api/save", HTTP_POST, handleSave);
    server.onNotFound(handleProvision);
    server.begin();
    Serial.printf("AP mode active, IP: %s\n", apIP.toString().c_str());
    lcd2.clear();
    lcd2.setCursor(0, 0);
    lcd2.print("AP: TempWatcher ");
    lcd2.setCursor(0, 1);
    lcd2.print(apIP.toString());
  } else {
    // NTP sync
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.setCursor(0, 0);
    lcd.print("Syncing time...");
    Serial.println("Syncing NTP...");
    configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
    struct tm ti;
    int ntpAttempts = 0;
    while (!getLocalTime(&ti) && ntpAttempts < 20) {
      delay(500);
      ntpAttempts++;
    }
    if (getLocalTime(&ti)) {
      char buf[20];
      strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &ti);
      Serial.printf("Time synced: %s\n", buf);
      lcd.setCursor(0, 1);
      lcd.print("OK: ");
      lcd.print(buf);
    } else {
      Serial.println("NTP sync failed, timestamps will be 0");
      lcd.setCursor(0, 1);
      lcd.print("NTP FAIL, continuing");
    }
    delay(1500);
    lcd.clear();

    // Register web routes
    server.on("/", handleRoot);
    server.on("/api", handleRoot);
    server.on("/api/data", handleApi);
    server.on("/api/stats", handleStats);
    server.on("/api/export", handleExport);
    server.on("/api/reset-flash", handleFlashReset);
    server.on("/api/wifi-setup", handleProvision);
    server.on("/api/scan", handleScan);
    server.on("/api/save", HTTP_POST, handleSave);
    server.begin();
    Serial.println("HTTP server started");

    // Show network info on lcd2
    strncpy(lcd2SsidVal, ssid, 32);
    lcd2SsidVal[32] = '\0';
    strncpy(lcd2IpVal, WiFi.localIP().toString().c_str(), 15);
    lcd2IpVal[15] = '\0';
    lcd2.clear();
    lcd2.setCursor(0, 0);
    lcd2.print("WiFi: ");
    lcd2.setCursor(6, 0);
    lcd2.print(lcd2SsidVal);
    lcd2.setCursor(0, 1);
    lcd2.print("IP: ");
    lcd2.setCursor(4, 1);
    lcd2.print(lcd2IpVal);
  }

  delay(2000);
  lcdDrawLabels();
}

// =============================================================================

void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();

  // Sensor read + LCD update every 500 ms
  if (millis() - lastLcdMs >= 500) {
    lastLcdMs = millis();

    // readAltitude() internally calls readPressure() again — compute from raw instead (~25 ms saved)
    temp = bmp.readTemperature();
    int32_t rawPressure = bmp.readPressure();
    pressureHPa = rawPressure / 100.0f;
    pressureMmHg = rawPressure / 133.322f;
    altitude = 44330.0f * (1.0f - powf((float)rawPressure / 101325.0f, 0.1903f));

    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);

    char buf[15];
    snprintf(buf, sizeof(buf), "%6.1f\xDFC  ", temp);
    lcd.setCursor(6, 0);
    lcd.print(buf);
    snprintf(buf, sizeof(buf), "%7.1f hPa", pressureHPa);
    lcd.setCursor(6, 1);
    lcd.print(buf);
    snprintf(buf, sizeof(buf), "%7.1f mmH", pressureMmHg);
    lcd.setCursor(6, 2);
    lcd.print(buf);
    snprintf(buf, sizeof(buf), "%8.1f m  ", altitude);
    lcd.setCursor(5, 3);
    lcd.print(buf);
  }

  // Physical reset button — hold 2 s to confirm
  if (digitalRead(RESET_BTN) == LOW) {
    delay(50);
    if (digitalRead(RESET_BTN) == LOW) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Hold 2s to reset");
      lcd.setCursor(0, 1);
      lcd.print("flash...");
      uint32_t holdStart = millis();
      while (digitalRead(RESET_BTN) == LOW) {
        delay(10);
        if (millis() - holdStart >= 2000) {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Resetting flash...");
          Serial.println("Physical button: flash reset");
          digitalWrite(WRITE_LED, HIGH);
          flash.eraseSector(0);
          writeIdx = 0;
          totalWritten = 0;
          metaSlot = 0;
          flashWriteMeta();
          digitalWrite(WRITE_LED, LOW);
          lcd.setCursor(0, 1);
          lcd.print("Done!");
          delay(1500);
          lcd.clear();
          break;
        }
      }
      if (digitalRead(RESET_BTN) == HIGH && millis() - holdStart < 2000) lcd.clear();
    }
  }

  // Save to flash every SAVE_INTERVAL
  if (millis() - lastSaveMs >= SAVE_INTERVAL) {
    flashSaveRecord();
    lastSaveMs = millis();
  }

  // WiFi watchdog — reconnect every WIFI_RETRY_INTERVAL when disconnected
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    if (!wifiWasLost) {
      wifiWasLost = true;
      wifiRetryCount = 0;
      wifiRetryMs = millis() - WIFI_RETRY_INTERVAL;  // first retry fires immediately
      Serial.println("WiFi connection lost!");
    }
    if (millis() - wifiRetryMs >= WIFI_RETRY_INTERVAL) {
      wifiRetryCount++;
      wifiRetryMs = millis();
      int wifiErr = WiFi.status();
      Serial.printf("WiFi retry #%u — %s\n", wifiRetryCount, wifiStatusToString(wifiErr));
      char buf[17];
      lcd2.clear();
      snprintf(buf, sizeof(buf), "WiFi:RETRY #%-4u", wifiRetryCount);
      lcd2.setCursor(0, 0);
      lcd2.print(buf);
      snprintf(buf, sizeof(buf), "%-16s", wifiStatusToString(wifiErr));
      lcd2.setCursor(0, 1);
      lcd2.print(buf);
      WiFi.disconnect();
      delay(100);
      WiFi.begin(ssid, password);
    }
  } else if (!apMode && wifiWasLost) {
    wifiWasLost = false;
    Serial.printf("WiFi reconnected after %u retries! IP: %s\n",
                  wifiRetryCount, WiFi.localIP().toString().c_str());
    wifiRetryCount = 0;
    configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
    strncpy(lcd2IpVal, WiFi.localIP().toString().c_str(), 15);
    lcd2IpVal[15] = '\0';
    lcd2SsidScroll = { 0, 1, 0 };
    lcd2IpScroll = { 0, 1, 0 };
    lcd2ScrollMs = 0;
    lcd2.clear();
    lcd2.setCursor(0, 0);
    lcd2.print("WiFi: ");
    lcd2.setCursor(6, 0);
    lcd2.print(lcd2SsidVal);
    lcd2.setCursor(0, 1);
    lcd2.print("IP: ");
    lcd2.setCursor(4, 1);
    lcd2.print(lcd2IpVal);
  }

  // Scroll lcd2 values — suppressed during WiFi loss and in AP mode
  if (!apMode && !wifiWasLost && millis() - lcd2ScrollMs >= 400) {
    lcd2ScrollMs = millis();
    lcd2ScrollTick(lcd2SsidScroll, lcd2SsidVal, 6, 0, 10);
    lcd2ScrollTick(lcd2IpScroll, lcd2IpVal, 4, 1, 12);
  }
}
