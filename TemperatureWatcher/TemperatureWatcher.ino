#include <Wire.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_BMP085.h>
#include <SPI.h>
#include <SPIMemory.h>

#include "config.h"
#include "html_pages.h"
#include "html_stats.h"

// Wi-Fi credentials — loaded from flash at runtime; configured via captive portal AP
char ssid[33]     = {};
char password[65] = {};

// Main LCD — sensor data (address 0x27)
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Second LCD 1602 — network info (A0 jumper soldered → address 0x26)
LiquidCrystal_I2C lcd2(0x26, 16, 2);

// BMP180 sensor (Adafruit BMP085 library is compatible with BMP180)
Adafruit_BMP085 bmp;

// LED blink state (toggled on every sensor read)
bool ledState = false;

// Web server on port 80
WebServer server(80);

// Sensor data (shared between display and web)
float temp = 0;
float pressureHPa = 0;
float pressureMmHg = 0;
float altitude = 0;

// ===== W25Q64 SPI Flash =====
// Wiring: MOSI=23, MISO=19, SCK=18, CS=5 (default ESP32 VSPI)
// All flash/timing constants are defined in config.h

SPIFlash flash(FLASH_CS);

struct CredsRecord {
  uint32_t magic;
  char     ssid[33];
  char     password[65];
};

struct Record {           // 16 bytes
  uint32_t timestamp;     // Unix time (Kyiv)
  float    temp;
  float    pressureHPa;
  float    altitude;
};

uint32_t writeIdx     = 0;
uint32_t totalWritten = 0;
uint16_t metaSlot     = 0;  // current slot index in sector 0 (0..META_SLOT_COUNT)
uint32_t lastSaveMs   = 0;
uint32_t lastLcdMs    = 0;
bool     flashOK      = false;

// AP mode / captive portal state
DNSServer dnsServer;
bool      apMode = false;

// Wi-Fi reconnect state (WIFI_RETRY_INTERVAL defined in config.h)
uint32_t wifiRetryMs    = 0;
uint16_t wifiRetryCount = 0;
bool     wifiWasLost    = false;

// LCD2 scroll state — prefixes "WiFi: " and "IP: " are static; only values scroll
char lcd2SsidVal[33] = {};  // SSID only, up to 32 chars
char lcd2IpVal[16]   = {};  // IP address only, up to 15 chars

struct ScrollTrack {
  int      pos     = 0;
  int      dir     = 1;
  uint32_t pauseMs = 0;  // non-zero while paused at an end
};

ScrollTrack lcd2SsidScroll;
ScrollTrack lcd2IpScroll;
uint32_t    lcd2ScrollMs = 0;

// Render one scrolling field on lcd2. Draws the visible window at (col, row) and
// advances the position, pausing 500 ms at each end before reversing.
// winSize — number of visible columns available for the value.
void lcd2ScrollTick(ScrollTrack& t, const char* val, uint8_t col, uint8_t row, int winSize) {
  int len = (int)strlen(val);
  if (len <= winSize) return;  // fits — nothing to scroll

  char win[17];
  strncpy(win, val + t.pos, winSize);
  win[winSize] = '\0';
  lcd2.setCursor(col, row);
  lcd2.print(win);

  int maxPos = len - winSize;
  if (t.pauseMs == 0) {
    t.pos += t.dir;
    if (t.pos >= maxPos) { t.pos = maxPos; t.dir = -1; t.pauseMs = millis(); }
    if (t.pos <= 0)      { t.pos = 0;      t.dir =  1; t.pauseMs = millis(); }
  } else if (millis() - t.pauseMs >= 500) {
    t.pauseMs = 0;
  }
}

// Draw static LCD labels once — never redrawn to avoid flicker
void lcdDrawLabels() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Temp:");
  lcd.setCursor(0, 1); lcd.print("hPa:");
  lcd.setCursor(0, 2); lcd.print("mmHg:");
  lcd.setCursor(0, 3); lcd.print("Alt:");
}

// ===== Flash helpers =====

void flashWriteMeta() {
  if (metaSlot >= META_SLOT_COUNT) {
    flash.eraseSector(0);
    metaSlot = 0;
  }
  uint32_t addr = (uint32_t)metaSlot * META_SLOT_SIZE;
  flash.writeAnything(addr, writeIdx);
  flash.writeAnything(addr + 4, totalWritten);
  metaSlot++;
}

void flashInit() {
  if (!flash.begin()) {
    Serial.println("W25Q64 not found!");
    return;
  }
  flashOK = true;
  Serial.printf("Flash OK, capacity: %u bytes\n", flash.getCapacity());
  digitalWrite(WRITE_LED, HIGH);

  // Binary search for the first empty slot in sector 0.
  // Slots are always written in order 0,1,2,..., so the layout is:
  //   [valid|valid|...|valid|empty|empty|...|empty]
  // An empty slot has both words == 0xFFFFFFFF (erased flash state).
  // log2(512) = 9 reads instead of up to 1024 sequential reads.
  uint16_t lo = 0, hi = META_SLOT_COUNT;
  while (lo < hi) {
    uint16_t mid = lo + (hi - lo) / 2;
    uint32_t wi, tw;
    flash.readAnything((uint32_t)mid * META_SLOT_SIZE,     wi);
    flash.readAnything((uint32_t)mid * META_SLOT_SIZE + 4, tw);
    if (wi == 0xFFFFFFFF && tw == 0xFFFFFFFF) hi = mid;
    else                                       lo = mid + 1;
  }
  metaSlot = lo;  // next free slot; lo==0 means fresh flash, lo==META_SLOT_COUNT means sector full

  if (lo == 0) {
    writeIdx = 0; totalWritten = 0;
    Serial.println("Flash: initialized fresh (capacity ~727 days at 2 min interval)");
  } else {
    uint32_t lastAddr = (uint32_t)(lo - 1) * META_SLOT_SIZE;
    flash.readAnything(lastAddr,     writeIdx);
    flash.readAnything(lastAddr + 4, totalWritten);
    Serial.printf("Flash: resuming at record %u (total %u), meta slot %u/%u\n",
                  writeIdx, totalWritten, metaSlot, (uint16_t)META_SLOT_COUNT);
  }
  digitalWrite(WRITE_LED, LOW);
}

// Read WiFi credentials from the last flash sector.
// Returns true and populates global ssid/password if magic matches.
bool readCredsFromFlash() {
  CredsRecord c;
  flash.readAnything(CREDS_ADDR, c);
  if (c.magic != CREDS_MAGIC) return false;
  c.ssid[32]     = '\0';
  c.password[64] = '\0';
  strncpy(ssid,     c.ssid,     32);  ssid[32]     = '\0';
  strncpy(password, c.password, 64);  password[64] = '\0';
  return true;
}

// Save WiFi credentials to the last flash sector (erases sector first).
void saveCredsToFlash(const char* s, const char* p) {
  CredsRecord c;
  c.magic = CREDS_MAGIC;
  memset(c.ssid,     0, sizeof(c.ssid));
  memset(c.password, 0, sizeof(c.password));
  strncpy(c.ssid,     s, 32);
  strncpy(c.password, p, 64);
  flash.eraseSector(CREDS_ADDR);
  flash.writeAnything(CREDS_ADDR, c);
}

void flashSaveRecord() {
  if (!flashOK) return;
  digitalWrite(WRITE_LED, HIGH);  // LED on while writing
  Record r = { (uint32_t)time(nullptr), temp, pressureHPa, altitude };
  uint32_t addr = RECORDS_ADDR + (uint32_t)writeIdx * RECORD_SIZE;
  if (writeIdx % 256 == 0) flash.eraseSector(addr);
  flash.writeAnything(addr, r);
  totalWritten++;
  writeIdx = (writeIdx + 1) % MAX_RECORDS;
  flashWriteMeta();
  digitalWrite(WRITE_LED, LOW);   // LED off when done
}

// Wi-Fi status code to human-readable string — const char* avoids heap allocation
const char* wifiStatusToString(int status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "Idle";
    case WL_NO_SSID_AVAIL:   return "SSID not found!";
    case WL_SCAN_COMPLETED:  return "Scan done";
    case WL_CONNECTED:       return "Connected!";
    case WL_CONNECT_FAILED:  return "Wrong password!";
    case WL_CONNECTION_LOST: return "Connection lost";
    case WL_DISCONNECTED:    return "Disconnected";
    default:                 return "Unknown";
  }
}

// Serve the main page — values updated via fetch('/api/data'), no full reload
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", HTML_ROOT);
}

// Statistics page — direct WiFiClient write with 2 KB coalescing buffer.
// Previously used ~100 tiny sendContent() calls (one per Bezier segment + one per table row),
// each becoming a separate HTTP chunk and TCP write. Now batched into ~6 large writes.
void handleStats() {
  if (!flashOK) { server.send(503, "text/plain", "Flash not available"); return; }

  uint32_t n = min((uint32_t)50, totalWritten);

  if (n == 0) {
    server.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
      "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;"
      "justify-content:center;align-items:center;min-height:100vh;margin:0}"
      "a{color:#e94560}</style></head>"
      "<body><p>No records yet. <a href='/'>Back</a></p></body></html>");
    return;
  }

  // Bulk flash read + reverse in place (oldest→newest → newest→oldest)
  Record recs[50];
  uint32_t startIdx = (writeIdx - n + MAX_RECORDS) % MAX_RECORDS;
  digitalWrite(WRITE_LED, HIGH);
  if (startIdx + n <= MAX_RECORDS) {
    flash.readByteArray(RECORDS_ADDR + startIdx * RECORD_SIZE, (uint8_t*)recs, n * RECORD_SIZE);
  } else {
    uint32_t fp = MAX_RECORDS - startIdx;
    flash.readByteArray(RECORDS_ADDR + startIdx * RECORD_SIZE, (uint8_t*)recs, fp * RECORD_SIZE);
    flash.readByteArray(RECORDS_ADDR, (uint8_t*)recs + fp * RECORD_SIZE, (n - fp) * RECORD_SIZE);
  }
  digitalWrite(WRITE_LED, LOW);
  for (uint32_t i = 0, j = n - 1; i < j; i++, j--) { Record t = recs[i]; recs[i] = recs[j]; recs[j] = t; }
  // recs[0]=newest, recs[n-1]=oldest

  float tmin = recs[0].temp, tmax = recs[0].temp;
  for (uint32_t i = 1; i < n; i++) {
    if (recs[i].temp < tmin) tmin = recs[i].temp;
    if (recs[i].temp > tmax) tmax = recs[i].temp;
  }

  // Direct client write — no chunked encoding overhead
  WiFiClient client = server.client();
  client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"));

  // 2 KB coalescing buffer: accumulate small strings, flush in large TCP writes
  char     outbuf[2048];
  uint16_t outlen = 0;

  auto flush = [&]() {
    if (outlen > 0) { client.write((const uint8_t*)outbuf, outlen); outlen = 0; yield(); }
  };
  // Append string to buffer; if string exceeds buffer size send it directly
  auto ap = [&](const char* s) {
    uint16_t l = (uint16_t)strlen(s);
    if (l >= sizeof(outbuf)) { flush(); client.write((const uint8_t*)s, l); yield(); return; }
    if (outlen + l >= sizeof(outbuf)) flush();
    memcpy(outbuf + outlen, s, l);
    outlen += l;
  };

  // ── Head + CSS (one large write) ────────────────────────────────────────────
  ap(HTML_STATS_HEAD);
  flush();  // send CSS before building dynamic content

  char buf[160];
  snprintf(buf, sizeof(buf),
    "<h1>Temperature History</h1>"
    "<div class='sub'>Stored: %u | Showing: %u records</div>",
    totalWritten, n);
  ap(buf);

  // ── SVG chart ───────────────────────────────────────────────────────────────
  ap("<svg viewBox='0 0 500 90' preserveAspectRatio='none' style='background:#0f0f1e;border-radius:8px'>");
  snprintf(buf, sizeof(buf), "<text x='4' y='13' font-size='10' fill='#555'>%.1f C</text><path d='", tmax);
  ap(buf);

  // Smooth curve via Catmull-Rom → cubic Bezier (recs[n-1]=oldest=left, recs[0]=newest=right)
  {
    float alpha = (n > 1) ? 490.0f / (float)(n - 1) : 0.0f;
    auto ptX = [&](int k) -> float { return (n < 2) ? 250.0f : k * alpha + 5.0f; };
    auto ptY = [&](int k) -> float {
      int idx = n - 1 - k;
      return (tmax == tmin) ? 40.0f : 5.0f + (tmax - recs[idx].temp) / (tmax - tmin) * 65.0f;
    };
    snprintf(buf, sizeof(buf), "M%d,%.1f", (n < 2 ? 250 : 5), ptY(0));
    ap(buf);
    for (int k = 0; k < (int)n - 1; k++) {
      int km1 = (k > 0)           ? k - 1 : 0;
      int k2  = (k + 2 < (int)n) ? k + 2 : n - 1;
      float cp1x = ptX(k)   + (ptX(k+1) - ptX(km1)) / 6.0f;
      float cp1y = ptY(k)   + (ptY(k+1) - ptY(km1)) / 6.0f;
      float cp2x = ptX(k+1) - (ptX(k2)  - ptX(k))   / 6.0f;
      float cp2y = ptY(k+1) - (ptY(k2)  - ptY(k))   / 6.0f;
      snprintf(buf, sizeof(buf), " C%.1f,%.1f %.1f,%.1f %.1f,%.1f",
               cp1x, cp1y, cp2x, cp2y, ptX(k+1), ptY(k+1));
      ap(buf);
    }
  }
  snprintf(buf, sizeof(buf),
    "' fill='none' stroke='#e94560' stroke-width='2'/>"
    "<text x='4' y='72' font-size='10' fill='#555'>%.1f C</text>"
    "<line x1='5' y1='76' x2='495' y2='76' stroke='#2a2a4a' stroke-width='1'/>", tmin);
  ap(buf);

  // Time axis labels
  {
    char tleft[14] = "-", tright[14] = "-";
    if (recs[n - 1].timestamp > 0) {
      time_t t = recs[n - 1].timestamp; struct tm ti; localtime_r(&t, &ti);
      strftime(tleft,  sizeof(tleft),  "%d.%m %H:%M", &ti);
    }
    if (recs[0].timestamp > 0) {
      time_t t = recs[0].timestamp; struct tm ti; localtime_r(&t, &ti);
      strftime(tright, sizeof(tright), "%d.%m %H:%M", &ti);
    }
    snprintf(buf, sizeof(buf),
      "<text x='5' y='87' font-size='9' fill='#666'>%s</text>"
      "<text x='495' y='87' font-size='9' fill='#666' text-anchor='end'>%s</text></svg>",
      tleft, tright);
    ap(buf);
  }
  flush();  // flush SVG before table

  // ── Table ───────────────────────────────────────────────────────────────────
  ap("<table><tr><th>#</th><th>Time</th><th>Temp</th><th>Pressure</th><th>Alt</th></tr>");
  for (uint32_t i = 0; i < n; i++) {
    char tbuf[20] = "-";
    if (recs[i].timestamp > 0) {
      time_t t = recs[i].timestamp; struct tm ti; localtime_r(&t, &ti);
      strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M", &ti);
    }
    snprintf(buf, sizeof(buf),
      "<tr><td>%u</td><td>%s</td><td class='v'>%.1f&deg;C</td><td>%.1f hPa / %.1f mmHg</td><td>%.1f m</td></tr>",
      totalWritten - i, tbuf, recs[i].temp, recs[i].pressureHPa, recs[i].pressureHPa / 1.33322f, recs[i].altitude);
    ap(buf);
  }

  ap(HTML_STATS_FOOT);
  flush();
  client.stop();
}

// Reset flash counters — erases only the metadata sector (sector 0).
// Data sectors are NOT erased here: flashSaveRecord() erases each sector
// automatically before first write, so old data is overwritten naturally.
// Erasing all data sectors in-request caused watchdog resets on large flash usage.
void handleFlashReset() {
  if (!flashOK) {
    server.send(503, "text/plain", "Flash not available");
    return;
  }
  digitalWrite(WRITE_LED, HIGH);
  flash.eraseSector(0);
  writeIdx     = 0;
  totalWritten = 0;
  metaSlot     = 0;
  flashWriteMeta();
  digitalWrite(WRITE_LED, LOW);
  Serial.println("Flash reset by user");
  server.sendHeader("Location", "/api/stats");
  server.send(303);
}

// CSV export — bypasses WebServer chunked encoding by writing directly to WiFiClient.
// Uses Connection: close so the browser knows EOF when the socket closes.
// Lines are batched into a 1 KB buffer; client.write() + yield() fires once per flush
// to keep TCP packets large and feed the watchdog / WiFi stack.
void handleExport() {
  if (!flashOK) { server.send(503, "text/plain", "Flash not available"); return; }

  uint32_t count    = min(totalWritten, (uint32_t)MAX_RECORDS);
  uint32_t startIdx = (writeIdx - count + MAX_RECORDS) % MAX_RECORDS;

  WiFiClient client = server.client();
  client.print(F("HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/csv\r\n"
                 "Content-Disposition: attachment; filename=\"data.csv\"\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "num,time,temperature_c,pressure_hpa,pressure_mmhg,altitude_m\r\n"));

  char     outbuf[1024];
  uint16_t outlen = 0;

  Record batch[16];
  digitalWrite(WRITE_LED, HIGH);  // stay on for entire export — individual reads are ~100 µs, invisible per-batch
  for (uint32_t i = 0; i < count; i += 16) {
    uint32_t batchSize  = min((uint32_t)16, count - i);
    uint32_t batchStart = (startIdx + i) % MAX_RECORDS;
    if (batchStart + batchSize <= MAX_RECORDS) {
      flash.readByteArray(RECORDS_ADDR + batchStart * RECORD_SIZE, (uint8_t*)batch, batchSize * RECORD_SIZE);
    } else {
      uint32_t fp = MAX_RECORDS - batchStart;
      flash.readByteArray(RECORDS_ADDR + batchStart * RECORD_SIZE, (uint8_t*)batch, fp * RECORD_SIZE);
      flash.readByteArray(RECORDS_ADDR, (uint8_t*)batch + fp * RECORD_SIZE, (batchSize - fp) * RECORD_SIZE);
    }
    for (uint32_t j = 0; j < batchSize; j++) {
      char tbuf[20] = "";
      if (batch[j].timestamp > 0) {
        time_t t = batch[j].timestamp;
        struct tm ti;
        localtime_r(&t, &ti);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
      }
      // Flush before writing: one CSV line is at most ~70 bytes
      if (outlen + 80 > sizeof(outbuf)) {
        client.write((const uint8_t*)outbuf, outlen);
        outlen = 0;
        yield();
      }
      outlen += snprintf(outbuf + outlen, sizeof(outbuf) - outlen,
        "%lu,\"%s\",%.1f,%.1f,%.1f,%.1f\r\n",
        (unsigned long)(i + j + 1),
        tbuf, batch[j].temp, batch[j].pressureHPa,
        batch[j].pressureHPa / 1.33322f, batch[j].altitude);
    }
  }
  if (outlen > 0) client.write((const uint8_t*)outbuf, outlen);
  digitalWrite(WRITE_LED, LOW);
  client.stop();
}

// JSON endpoint — snprintf into fixed buffer, zero heap allocations
void handleApi() {
  server.sendHeader("Cache-Control", "no-cache");
  char json[96];
  snprintf(json, sizeof(json), "{\"temperature_c\":%.1f,\"pressure_hpa\":%.1f,\"pressure_mmhg\":%.1f,\"altitude_m\":%.1f}", temp, pressureHPa, pressureMmHg, altitude);
  server.send(200, "application/json", json);
}

// ===== Captive portal provisioning handlers (AP mode only) =====

void handleProvision() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", HTML_PROVISION);
}

void handleScan() {
  int n = WiFi.scanNetworks();
  // Fixed stack buffer — no heap String reallocs. 30 networks × ~70 bytes/entry + brackets < 2200 bytes.
  char json[2200];
  uint16_t len = 0;
  json[len++] = '[';
  for (int i = 0; i < n; i++) {
    if (len > sizeof(json) - 80) break;  // guard: stop before buffer full
    if (i > 0) json[len++] = ',';
    String s = WiFi.SSID(i);
    s.replace("\"", "\\\"");
    bool enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    len += snprintf(json + len, sizeof(json) - len,
                    "{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d}",
                    s.c_str(), WiFi.RSSI(i), enc ? 1 : 0);
  }
  json[len++] = ']';
  json[len]   = '\0';
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", json);
}

void handleSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing ssid");
    return;
  }
  String s = server.arg("ssid");
  String p = server.arg("password");
  saveCredsToFlash(s.c_str(), p.c_str());
  server.send(200, "text/plain", "OK");
  delay(500);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_BTN, INPUT_PULLUP);
  pinMode(WRITE_LED, OUTPUT);
  digitalWrite(WRITE_LED, LOW);

  // Initialize W25Q64 flash
  flashInit();

  // Load WiFi credentials from flash — if none saved yet, AP mode will start below
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

  // ===== Splash screen =====
  lcd.setCursor(3, 1);
  lcd.print("BMP180 Station");
  delay(1500);
  lcd.clear();

  // ===== Initialize BMP180 =====
  lcd.setCursor(0, 0);
  lcd.print("[1/2] Sensor...");
  Serial.println("Initializing BMP180...");

  if (!bmp.begin()) {
    lcd.setCursor(0, 1);
    lcd.print("FAIL: BMP180 missing");
    Serial.println("BMP180 not found!");
    while (true) { 
      delay(1000); 
    }
  }

  lcd.setCursor(0, 1);
  lcd.print("OK: BMP180 ready");
  Serial.println("BMP180 OK");
  delay(1000);

  // ===== Connect to Wi-Fi =====
  lcd.setCursor(0, 2);
  lcd.print("[2/2] WiFi...");

  int attempts = 0;
  int lastStatus = -1;

  if (ssid[0] != '\0') {
    // Credentials available — attempt connection
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

    // Update LCD only when status changes or every 5 attempts
    if (status != lastStatus || attempts % 5 == 0) {
      lcd.setCursor(0, 3);
      lcd.print("                    ");  // clear row
      lcd.setCursor(0, 3);
      lcd.print(wifiStatusToString(status));
      lcd.print(" (");
      lcd.print(attempts);
      lcd.print("/15)");
      lastStatus = status;
    }

    Serial.printf("  Attempt %d/15 - Status: %s (%d)\n",
                  attempts, wifiStatusToString(status), status);

    // Blink LED while connecting
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  // ===== Check result =====
  lcd.clear();

  if (WiFi.status() != WL_CONNECTED) {
    // 15 attempts failed — start captive portal AP mode
    Serial.println("WiFi FAILED — starting AP captive portal");
    apMode = true;

    WiFi.mode(WIFI_AP_STA);
    IPAddress apIP(192, 168, 0, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("TempWatcher");

    dnsServer.start(53, "*", apIP);

    // Register provisioning routes only
    server.on("/",     handleProvision);
    server.on("/scan", handleScan);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleProvision);
    server.begin();

    Serial.printf("AP mode active, IP: %s\n", apIP.toString().c_str());

    // Show AP info on lcd2 (16x2)
    lcd2.clear();
    lcd2.setCursor(0, 0); lcd2.print("AP: TempWatcher ");
    lcd2.setCursor(0, 1); lcd2.print(apIP.toString());
  } else {
    // ===== WiFi connected =====
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // ===== NTP time sync (Kyiv: UTC+2 winter / UTC+3 summer) =====
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

    // Setup normal web server routes
    server.on("/", handleRoot);
    server.on("/api/data", handleApi);
    server.on("/api/stats", handleStats);
    server.on("/api/export", handleExport);
    server.on("/api/reset-flash", handleFlashReset);
    server.on("/wifi-setup", handleProvision);
    server.on("/scan", handleScan);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();

    Serial.println("HTTP server started");

    // Show network info on second LCD 1602
    // Row 0: "WiFi: " + SSID — scrolls in loop() if longer than 10 chars
    // Row 1: "IP: <address>"  — scrolls in loop() if longer than 12 chars
    strncpy(lcd2SsidVal, ssid, 32);
    lcd2SsidVal[32] = '\0';
    lcd2.clear();
    lcd2.setCursor(0, 0); lcd2.print("WiFi: ");
    lcd2.setCursor(6, 0); lcd2.print(lcd2SsidVal);
    strncpy(lcd2IpVal, WiFi.localIP().toString().c_str(), 15);
    lcd2IpVal[15] = '\0';
    lcd2.setCursor(0, 1); lcd2.print("IP: ");
    lcd2.setCursor(4, 1); lcd2.print(lcd2IpVal);
  }

  delay(2000);
  lcdDrawLabels();
}

void loop() {
  // In AP mode: process DNS redirects for captive portal
  if (apMode) dnsServer.processNextRequest();

  // Handle incoming HTTP requests
  server.handleClient();

  // Update sensor + LCD every 500 ms without blocking
  if (millis() - lastLcdMs >= 500) {
    lastLcdMs = millis();

    // Read sensor — readAltitude() internally calls readPressure() again,
    // so compute altitude from the already-read rawPressure instead (saves one full BMP180 cycle, ~25 ms)
    temp = bmp.readTemperature();
    int32_t rawPressure = bmp.readPressure();
    pressureHPa  = rawPressure / 100.0f;
    pressureMmHg = rawPressure / 133.322f;
    altitude     = 44330.0f * (1.0f - powf((float)rawPressure / 101325.0f, 0.1903f));

    // Toggle LED on each sensor read
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);

    // Write only values at fixed positions with fixed width — no flicker
    char buf[15];

    // Row 0: "Temp: " + "-99.9 C  " (col 6, 9 chars)
    snprintf(buf, sizeof(buf), "%6.1f\xDFC  ", temp);
    lcd.setCursor(6, 0); lcd.print(buf);

    // Row 1: "hPa:  " + "1013.2 hPa" (col 6, 10 chars)
    snprintf(buf, sizeof(buf), "%7.1f hPa", pressureHPa);
    lcd.setCursor(6, 1); lcd.print(buf);

    // Row 2: "mmHg: " + " 760.0 mmHg" (col 6, 10 chars)
    snprintf(buf, sizeof(buf), "%7.1f mmH", pressureMmHg);
    lcd.setCursor(6, 2); lcd.print(buf);

    // Row 3: "Alt:  " + " 123.4 m  " (col 5, 11 chars)
    snprintf(buf, sizeof(buf), "%8.1f m  ", altitude);
    lcd.setCursor(5, 3); lcd.print(buf);
  }

  // Physical flash reset button: hold 2 seconds to confirm
  // Crunch as for me 
  if (digitalRead(RESET_BTN) == LOW) {
    delay(50);                          // debounce
    if (digitalRead(RESET_BTN) == LOW) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Hold 2s to reset");
      lcd.setCursor(0, 1);
      lcd.print("flash...");
      uint32_t holdStart = millis();
      while (digitalRead(RESET_BTN) == LOW) {
        delay(10);  // feed watchdog, avoid tight-loop reset
        if (millis() - holdStart >= 2000) {
          // Confirmed — erase
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Resetting flash...");
          Serial.println("Physical button: flash reset");
          digitalWrite(WRITE_LED, HIGH);
          flash.eraseSector(0);  // metadata sector only — data sectors erased on next write
          writeIdx = 0; totalWritten = 0; metaSlot = 0;
          flashWriteMeta();
          digitalWrite(WRITE_LED, LOW);
          lcd.setCursor(0, 1);
          lcd.print("Done!");
          delay(1500);
          lcd.clear();
          break;
        }
      }
      // Released before 2s — cancelled
      if (digitalRead(RESET_BTN) == HIGH && millis() - holdStart < 2000) {
        lcd.clear();
      }
    }
  }

  // Save to flash on interval
  if (millis() - lastSaveMs >= SAVE_INTERVAL) {
    flashSaveRecord();
    lastSaveMs = millis();
  }

  // ===== Wi-Fi watchdog — reconnect every WIFI_RETRY_INTERVAL when disconnected =====
  // Skipped in AP mode — reconnect is handled via captive portal provisioning
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    if (!wifiWasLost) {
      wifiWasLost    = true;
      wifiRetryCount = 0;
      wifiRetryMs    = millis() - WIFI_RETRY_INTERVAL;  // first retry fires immediately
      Serial.println("WiFi connection lost!");
    }
    if (millis() - wifiRetryMs >= WIFI_RETRY_INTERVAL) {
      wifiRetryCount++;
      wifiRetryMs = millis();
      int wifiErr = WiFi.status();
      Serial.printf("WiFi retry #%u — %s\n", wifiRetryCount, wifiStatusToString(wifiErr));
      // Row 0: "WiFi:RETRY #NNN " (16 chars)
      // Row 1: error string, left-padded to 16 chars
      char buf[17];
      lcd2.clear();
      snprintf(buf, sizeof(buf), "WiFi:RETRY #%-4u", wifiRetryCount);
      lcd2.setCursor(0, 0); lcd2.print(buf);
      snprintf(buf, sizeof(buf), "%-16s", wifiStatusToString(wifiErr));
      lcd2.setCursor(0, 1); lcd2.print(buf);
      // Attempt reconnect
      WiFi.disconnect();
      delay(100);
      WiFi.begin(ssid, password);
    }
  } else if (!apMode && wifiWasLost) {
    // Just reconnected
    wifiWasLost = false;
    Serial.printf("WiFi reconnected after %u retries! IP: %s\n",
                  wifiRetryCount, WiFi.localIP().toString().c_str());
    wifiRetryCount = 0;
    // Re-sync NTP (was skipped while offline)
    configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
    // Restore lcd2 to normal network display
    strncpy(lcd2IpVal, WiFi.localIP().toString().c_str(), 15);
    lcd2IpVal[15] = '\0';
    lcd2SsidScroll = {0, 1, 0};
    lcd2IpScroll   = {0, 1, 0};
    lcd2ScrollMs   = 0;
    lcd2.clear();
    lcd2.setCursor(0, 0); lcd2.print("WiFi: ");
    lcd2.setCursor(0, 1); lcd2.print("IP: ");
    lcd2.setCursor(6, 0); lcd2.print(lcd2SsidVal);
    lcd2.setCursor(4, 1); lcd2.print(lcd2IpVal);
  }

  // Scroll LCD2 values if they overflow their column window (bounce left↔right every 400 ms)
  // Row 0: "WiFi: " fixed at cols 0-5, SSID scrolls in cols 6-15 (10 chars)
  // Row 1: "IP: "   fixed at cols 0-3, IP   scrolls in cols 4-15 (12 chars)
  // Suppressed during WiFi loss and in AP mode — status messages must stay visible
  if (!apMode && !wifiWasLost && millis() - lcd2ScrollMs >= 400) {
    lcd2ScrollMs = millis();
    lcd2ScrollTick(lcd2SsidScroll, lcd2SsidVal, 6, 0, 10);
    lcd2ScrollTick(lcd2IpScroll,   lcd2IpVal,   4, 1, 12);
  }
}
