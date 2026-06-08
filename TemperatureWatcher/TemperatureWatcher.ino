#include <WiFi.h>
#include <DNSServer.h>
#include <Adafruit_BMP085.h>

#include <esp_task_wdt.h>
#include <ArduinoOTA.h>

#include "config.h"
#include "globals.h"
#include "flash.h"
#include "lcd_display.h"
#include "web_handlers.h"
#include "tasks.h"
#include "mqtt_handler.h"

// ===== Sensor data — written here (Core 1), read by taskWeb (Core 0) =====
volatile float  temp         = 0;
volatile float  pressureHPa  = 0;
volatile float  pressureMmHg = 0;
volatile float  altitude     = 0;
volatile int8_t pressureTrend = 0;   // -1 falling, 0 stable, +1 rising
volatile bool   sensorOK     = false;

// 24-hour min/max (Core 1 only — read by handleApi which casts volatile read)
float tempMin24h = 0.0f;
float tempMax24h = 0.0f;

// ===== WiFi credentials — loaded from flash; configured via captive portal =====
char ssid[33]     = {};
char password[65] = {};

// ===== Runtime state =====
Adafruit_BMP085 bmp;
DNSServer dnsServer;
volatile bool apMode = false;
bool ledState        = false;

uint32_t lastSaveMs      = 0;
uint32_t lastLcdMs       = 0;
uint32_t wifiRetryMs     = 0;
uint32_t wifiPeriodicMs  = 0;
uint16_t wifiRetryCount  = 0;
bool wifiWasLost         = false;

// =============================================================================

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,   OUTPUT);
  pinMode(WRITE_LED, OUTPUT);
  digitalWrite(WRITE_LED, LOW);

  flashInit();

  // Init watchdog timer now, but don't add this task yet —
  // setup() can take >30 s (WiFi + NTP delays) and would trip the watchdog.
  // esp_task_wdt_add(NULL) is called at the very end of setup().
  {
    esp_task_wdt_config_t wdtCfg = {
      .timeout_ms     = WDT_TIMEOUT_S * 1000U,
      .idle_core_mask = 0,
      .trigger_panic  = true,
    };
    esp_task_wdt_init(&wdtCfg);
  }

  if (flashOK && readCredsFromFlash()) {
    Serial.printf("Loaded credentials from flash: SSID=%s\n", ssid);
  } else {
    Serial.println("No credentials in flash — will start captive portal AP");
  }

  lcd.init();  lcd.backlight();
  lcd2.init(); lcd2.backlight();

  lcd.setCursor(3, 1);
  lcd.print("BMP180 Station");
  delay(1500);
  lcd.clear();

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
    Serial.println("WiFi FAILED — starting AP captive portal");
    apMode = true;
    IPAddress apIP(192, 168, 0, 1);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("TempWatcher");
    dnsServer.start(53, "*", apIP);
    server.on("/",         HTTP_GET,  handleProvision);
    server.on("/api/scan", HTTP_GET,  handleScan);
    server.on("/api/save", HTTP_POST, handleSave);
    server.onNotFound(handleProvision);
    server.begin();
    Serial.printf("AP mode active, IP: %s\n", apIP.toString().c_str());
    lcd2.clear();
    lcd2.setCursor(0, 0); lcd2.print("AP: TempWatcher ");
    lcd2.setCursor(0, 1); lcd2.print(apIP.toString());
  } else {
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.setCursor(0, 0);
    lcd.print("Syncing time...");
    Serial.println("Syncing NTP...");
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
    struct tm ti;
    int ntpAttempts = 0;
    while (!getLocalTime(&ti) && ntpAttempts < 20) { delay(500); ntpAttempts++; }
    if (getLocalTime(&ti)) {
      char buf[20];
      strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &ti);
      Serial.printf("Time synced: %s\n", buf);
      lcd.setCursor(0, 1); lcd.print("OK: "); lcd.print(buf);
    } else {
      Serial.println("NTP sync failed, timestamps will be 0");
      lcd.setCursor(0, 1); lcd.print("NTP FAIL, continuing");
    }
    delay(1500);
    lcd.clear();

    server.on("/",                HTTP_GET,  handleRoot);
    server.on("/api/data",        HTTP_GET,  handleApi);
    server.on("/api/stats",       HTTP_GET,  handleStats);
    server.on("/api/export",      HTTP_GET,  handleExport);
    server.on("/api/reset-flash", HTTP_GET,  handleFlashReset);
    server.on("/api/reboot",      HTTP_GET,  handleReboot);
    server.on("/api/wifi-setup",  HTTP_GET,  handleProvision);
    server.on("/api/scan",        HTTP_GET,  handleScan);
    server.on("/api/save",        HTTP_POST, handleSave);
    server.begin();
    Serial.println("HTTP server started");

    ArduinoOTA.setHostname("tempwatcher");
    ArduinoOTA.onStart([]() {
      // Remove loop task from watchdog before OTA blocks it.
      esp_task_wdt_delete(NULL);
      Serial.println("OTA start");
    });
    ArduinoOTA.onError([](ota_error_t e) {
      Serial.printf("OTA error %u\n", e);
      esp_task_wdt_add(NULL);  // re-arm watchdog if OTA failed
    });
    ArduinoOTA.begin();
    Serial.println("OTA ready");

    mqttSetup();

    strncpy(lcd2SsidVal, ssid, 32); lcd2SsidVal[32] = '\0';
    strncpy(lcd2IpVal, WiFi.localIP().toString().c_str(), 15); lcd2IpVal[15] = '\0';
    lcd2.clear();
    lcd2.setCursor(0, 0); lcd2.print("WiFi: "); lcd2.print(lcd2SsidVal);
    lcd2.setCursor(0, 1); lcd2.print("IP: ");   lcd2.print(lcd2IpVal);
  }

  delay(2000);
  lcdDrawLabels();

  // Setup is complete — now arm the watchdog for loop().
  // setup() can take >30 s (WiFi retries + NTP), so we only register here.
  esp_task_wdt_add(NULL);

  // Web server on Core 0; this loop (Core 1) owns sensors, LCD, flash, WiFi watchdog.
  xTaskCreatePinnedToCore(taskWeb, "WebTask", 12288, nullptr, 1, nullptr, 0);
}

// =============================================================================

void loop() {
  esp_task_wdt_reset();

  // Deferred restart requested by an async handler (e.g. credential save / reboot),
  // performed here so the HTTP response has time to flush first.
  if (g_rebootAtMs && millis() >= g_rebootAtMs) {
    ESP.restart();
  }

  // Sensor read + LCD update every 500 ms
  if (millis() - lastLcdMs >= 500) {
    lastLcdMs = millis();

    float   newT = bmp.readTemperature();
    int32_t rawP = bmp.readPressure();
    static bool firstReading = true;
    static bool minMaxInit   = false;

    if (!isnan(newT) && rawP > 0) {
      sensorOK = true;
      if (firstReading) {
        temp = newT; pressureHPa = rawP / 100.0f; firstReading = false;
      } else {
        temp        = temp        * (1.0f - SENSOR_EMA_ALPHA) + newT           * SENSOR_EMA_ALPHA;
        pressureHPa = pressureHPa * (1.0f - SENSOR_EMA_ALPHA) + rawP / 100.0f * SENSOR_EMA_ALPHA;
      }
      pressureMmHg = pressureHPa / 1.33322f;
      altitude     = 44330.0f * (1.0f - powf((float)pressureHPa / 1013.25f, 0.1903f));

      float t = (float)temp;
      if (!minMaxInit || t < tempMin24h) tempMin24h = t;
      if (!minMaxInit || t > tempMax24h) tempMax24h = t;
      minMaxInit = true;
    } else {
      sensorOK = false;
    }

    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);

    char buf[15];
    snprintf(buf, sizeof(buf), "%6.1f\xDFC  ", (float)temp);
    lcd.setCursor(6, 0); lcd.print(buf);
    char trendChar = (pressureTrend > 0) ? '^' : (pressureTrend < 0) ? 'v' : '-';
    snprintf(buf, sizeof(buf), "%7.1f hPa%c", (float)pressureHPa, trendChar);
    lcd.setCursor(6, 1); lcd.print(buf);
    snprintf(buf, sizeof(buf), "%7.1f mmH", (float)pressureMmHg);
    lcd.setCursor(6, 2); lcd.print(buf);
    snprintf(buf, sizeof(buf), "%8.1f m  ", (float)altitude);
    lcd.setCursor(5, 3); lcd.print(buf);
  }

  // Save to flash every SAVE_INTERVAL
  if (millis() - lastSaveMs >= SAVE_INTERVAL) {
    flashSaveRecord();
    lastSaveMs = millis();
  }

  // Periodic proactive reconnect — every 5 min even when connected, refreshes IP lease and LCD2
  if (!apMode && WiFi.status() == WL_CONNECTED &&
      millis() - wifiPeriodicMs >= WIFI_PERIODIC_INTERVAL) {
    wifiPeriodicMs = millis();
    Serial.println("Periodic WiFi reconnect triggered");
    WiFi.disconnect();
  }

  // WiFi watchdog — reconnect every WIFI_RETRY_INTERVAL when disconnected
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    if (!wifiWasLost) {
      wifiWasLost    = true;
      wifiRetryCount = 0;
      wifiRetryMs    = millis() - WIFI_RETRY_INTERVAL;  // first retry fires immediately
      Serial.println("WiFi connection lost!");
    }
    if (millis() - wifiRetryMs >= WIFI_RETRY_INTERVAL) {
      wifiRetryMs = millis();
      int wifiErr = WiFi.status();
      char buf[17];
      lcd2.clear();

      if (wifiRetryCount < WIFI_HARD_RESET_AFTER) {
        // Soft reconnect — standard sequence
        Serial.printf("WiFi retry #%u (soft) — %s\n", wifiRetryCount, wifiStatusToString(wifiErr));
        snprintf(buf, sizeof(buf), "WiFi:RETRY #%-4u", wifiRetryCount);
        lcd2.setCursor(0, 0); lcd2.print(buf);
        snprintf(buf, sizeof(buf), "%-16s", wifiStatusToString(wifiErr));
        lcd2.setCursor(0, 1); lcd2.print(buf);
        WiFi.disconnect();
        delay(200);
        WiFi.begin(ssid, password);
      } else {
        // Hard reset — full radio cycle to clear stuck driver state
        Serial.printf("WiFi retry #%u (hard reset) — %s\n", wifiRetryCount, wifiStatusToString(wifiErr));
        snprintf(buf, sizeof(buf), "WiFi:RST  #%-4u", wifiRetryCount);
        lcd2.setCursor(0, 0); lcd2.print(buf);
        lcd2.setCursor(0, 1); lcd2.print("Radio full reset");
        WiFi.disconnect();
        delay(500);
        WiFi.mode(WIFI_OFF);
        delay(1000);
        WiFi.mode(WIFI_STA);
        delay(200);
        WiFi.begin(ssid, password);
      }

      wifiRetryCount++;
    }
  } else if (!apMode && wifiWasLost) {
    wifiWasLost = false;
    Serial.printf("WiFi reconnected after %u retries! IP: %s\n",
                  wifiRetryCount, WiFi.localIP().toString().c_str());
    wifiRetryCount = 0;
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
    strncpy(lcd2IpVal, WiFi.localIP().toString().c_str(), 15); lcd2IpVal[15] = '\0';
    lcd2SsidScroll = { 0, 1, 0 };
    lcd2IpScroll   = { 0, 1, 0 };
    lcd2ScrollMs   = 0;
    lcd2.clear();
    lcd2.setCursor(0, 0); lcd2.print("WiFi: "); lcd2.print(lcd2SsidVal);
    lcd2.setCursor(0, 1); lcd2.print("IP: ");   lcd2.print(lcd2IpVal);
  }

  // Scroll lcd2 values — suppressed during WiFi loss and in AP mode
  if (!apMode && !wifiWasLost && millis() - lcd2ScrollMs >= 400) {
    lcd2ScrollMs = millis();
    lcd2ScrollTick(lcd2SsidScroll, lcd2SsidVal, 6, 0, 10);
    lcd2ScrollTick(lcd2IpScroll,   lcd2IpVal,   4, 1, 12);
  }

  // Pressure trend — compare current sample vs one taken TREND_SAMPLE_INTERVAL ago
  {
    static float    pTrendPrev = 0.0f;
    static uint32_t pTrendMs   = 0;
    if (millis() - pTrendMs >= TREND_SAMPLE_INTERVAL) {
      pTrendMs = millis();
      float pNow = (float)pressureHPa;
      if (pTrendPrev > 0.0f) {
        float diff = pNow - pTrendPrev;
        pressureTrend = (diff >  TREND_THRESHOLD_HPA) ?  1 :
                        (diff < -TREND_THRESHOLD_HPA) ? -1 : 0;
      }
      pTrendPrev = pNow;
    }
  }

  // 24-hour min/max daily reset
  {
    static uint32_t minMaxResetMs = 0;
    if (millis() - minMaxResetMs >= MINMAX_RESET_INTERVAL) {
      minMaxResetMs = millis();
      tempMin24h = (float)temp;
      tempMax24h = (float)temp;
    }
  }

  if (!apMode) ArduinoOTA.handle();
  mqttLoop((float)temp, (float)pressureHPa, (float)pressureMmHg, (float)altitude,
           tempMin24h, tempMax24h, pressureTrend, millis() / 1000);
}
