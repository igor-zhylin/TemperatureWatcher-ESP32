#include "web_handlers.h"
#include <WiFi.h>
#include <memory>
#include "flash.h"
#include "globals.h"
#include "config.h"
#include "html_pages.h"
#include "html_stats.h"
#include <time.h>

AsyncWebServer server(80);
volatile uint32_t g_rebootAtMs = 0;

void handleRoot(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *res = request->beginResponse_P(200, "text/html", HTML_ROOT);
  res->addHeader("Cache-Control", "no-cache");
  request->send(res);
}

void handleApi(AsyncWebServerRequest *request) {
  char json[256];
  snprintf(json, sizeof(json),
           "{\"temperature_c\":%.1f,\"pressure_hpa\":%.1f,\"pressure_mmhg\":%.1f,\"altitude_m\":%.1f,"
           "\"trend\":%d,\"temp_min_24h\":%.1f,\"temp_max_24h\":%.1f,"
           "\"uptime_s\":%lu,\"rssi\":%d,\"sensor_ok\":%d,"
           "\"free_heap\":%lu,\"min_free_heap\":%lu}",
           (float)temp, (float)pressureHPa, (float)pressureMmHg, (float)altitude,
           (int)pressureTrend, tempMin24h, tempMax24h,
           (unsigned long)(millis() / 1000), WiFi.RSSI(), sensorOK ? 1 : 0,
           (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
  AsyncWebServerResponse *res = request->beginResponse(200, "application/json", json);
  res->addHeader("Cache-Control", "no-cache");
  request->send(res);
}

void handleStats(AsyncWebServerRequest *request) {
  if (!flashOK) {
    request->send(503, "text/plain", "Flash not available");
    return;
  }

  // Optional pagination offset (0 = newest records)
  uint32_t offset = 0;
  if (request->hasParam("offset")) {
    long v = request->getParam("offset")->value().toInt();
    if (v > 0) offset = (uint32_t)v;
  }

  // Snapshot ring-buffer position under mutex so n and startIdx are consistent.
  xSemaphoreTake(flashMutex, portMAX_DELAY);
  uint32_t tw = totalWritten;
  uint32_t wi = writeIdx;
  xSemaphoreGive(flashMutex);

  if (tw == 0 || offset >= tw) {
    request->send(200, "text/html",
                  "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;"
                  "justify-content:center;align-items:center;min-height:100vh;margin:0}"
                  "a{color:#e94560}</style></head>"
                  "<body><p>No records yet. <a href='/'>Back</a></p></body></html>");
    return;
  }

  uint32_t avail    = tw - offset;
  uint32_t n        = (avail < 50) ? avail : 50;
  uint32_t startIdx = (wi - offset - n + (uint32_t)MAX_RECORDS * 2) % MAX_RECORDS;

  // Bulk flash read — wrap-around aware, mutex-protected against flashSaveRecord().
  Record recs[50];
  xSemaphoreTake(flashMutex, portMAX_DELAY);
  digitalWrite(WRITE_LED, HIGH);
  if (startIdx + n <= MAX_RECORDS) {
    flash.readByteArray(RECORDS_ADDR + startIdx * RECORD_SIZE, (uint8_t*)recs, n * RECORD_SIZE);
  } else {
    uint32_t fp = MAX_RECORDS - startIdx;
    flash.readByteArray(RECORDS_ADDR + startIdx * RECORD_SIZE, (uint8_t*)recs, fp * RECORD_SIZE);
    flash.readByteArray(RECORDS_ADDR, (uint8_t*)recs + fp * RECORD_SIZE, (n - fp) * RECORD_SIZE);
  }
  digitalWrite(WRITE_LED, LOW);
  xSemaphoreGive(flashMutex);

  // Reverse in place (newest first) — local RAM copy, no mutex needed.
  for (uint32_t i = 0, j = n - 1; i < j; i++, j--) {
    Record t = recs[i]; recs[i] = recs[j]; recs[j] = t;
  }

  float tmin = recs[0].temp, tmax = recs[0].temp;
  for (uint32_t i = 1; i < n; i++) {
    if (recs[i].temp < tmin) tmin = recs[i].temp;
    if (recs[i].temp > tmax) tmax = recs[i].temp;
  }

  // Build the page in RAM (bounded: ~50 rows + 2 charts ≈ 12 KB) and let the
  // async server stream it. Heavy lifting (flash) is already done above.
  String h;
  h.reserve(16384);
  h += FPSTR(HTML_STATS_HEAD);

  // 256, not 192: the pressure chart's closing line (<line> + two <text> + </svg>)
  // is ~226 chars; a smaller buffer truncated </svg>, leaving the SVG open and
  // swallowing the records table.
  char buf[256];
  snprintf(buf, sizeof(buf),
           "<h1>Temperature History</h1>"
           "<div class='sub'>Stored: %u | Showing records %u&ndash;%u</div>",
           tw, tw - offset, tw - offset - n + 1);
  h += buf;

  // ---- Temperature chart ----
  h += "<p style='font-size:.85em;color:#888;margin-bottom:4px'>Temperature (&deg;C)</p>";
  h += "<svg viewBox='0 0 500 90' preserveAspectRatio='none' style='background:#0f0f1e;border-radius:8px'>";
  snprintf(buf, sizeof(buf), "<text x='4' y='13' font-size='10' fill='#555'>%.1f C</text><path d='", tmax);
  h += buf;
  {
    float alpha = (n > 1) ? 490.0f / (float)(n - 1) : 0.0f;
    auto ptX = [&](int k) -> float { return (n < 2) ? 250.0f : k * alpha + 5.0f; };
    auto ptY = [&](int k) -> float {
      int idx = n - 1 - k;
      return (tmax == tmin) ? 40.0f : 5.0f + (tmax - recs[idx].temp) / (tmax - tmin) * 65.0f;
    };
    snprintf(buf, sizeof(buf), "M%d,%.1f", (n < 2 ? 250 : 5), ptY(0));
    h += buf;
    for (int k = 0; k < (int)n - 1; k++) {
      int km1 = (k > 0) ? k - 1 : 0;
      int k2  = (k + 2 < (int)n) ? k + 2 : n - 1;
      float cp1x = ptX(k)     + (ptX(k + 1) - ptX(km1)) / 6.0f;
      float cp1y = ptY(k)     + (ptY(k + 1) - ptY(km1)) / 6.0f;
      float cp2x = ptX(k + 1) - (ptX(k2)    - ptX(k))   / 6.0f;
      float cp2y = ptY(k + 1) - (ptY(k2)    - ptY(k))   / 6.0f;
      snprintf(buf, sizeof(buf), " C%.1f,%.1f %.1f,%.1f %.1f,%.1f",
               cp1x, cp1y, cp2x, cp2y, ptX(k + 1), ptY(k + 1));
      h += buf;
    }
  }
  snprintf(buf, sizeof(buf),
           "' fill='none' stroke='#e94560' stroke-width='2'/>"
           "<text x='4' y='72' font-size='10' fill='#555'>%.1f C</text>",
           tmin);
  h += buf;
  h += "<line x1='5' y1='76' x2='495' y2='76' stroke='#2a2a4a' stroke-width='1'/>";
  {
    char tleft[14] = "-", tright[14] = "-";
    if (recs[n - 1].timestamp > 0) {
      time_t t = recs[n - 1].timestamp; struct tm ti; localtime_r(&t, &ti);
      strftime(tleft, sizeof(tleft), "%d.%m %H:%M", &ti);
    }
    if (recs[0].timestamp > 0) {
      time_t t = recs[0].timestamp; struct tm ti; localtime_r(&t, &ti);
      strftime(tright, sizeof(tright), "%d.%m %H:%M", &ti);
    }
    snprintf(buf, sizeof(buf),
             "<text x='5' y='87' font-size='9' fill='#666'>%s</text>"
             "<text x='495' y='87' font-size='9' fill='#666' text-anchor='end'>%s</text></svg>",
             tleft, tright);
    h += buf;
  }

  // ---- Pressure chart ----
  {
    float pmin = recs[0].pressureHPa, pmax = recs[0].pressureHPa;
    for (uint32_t i = 1; i < n; i++) {
      if (recs[i].pressureHPa < pmin) pmin = recs[i].pressureHPa;
      if (recs[i].pressureHPa > pmax) pmax = recs[i].pressureHPa;
    }
    h += "<p style='font-size:.85em;color:#888;margin-bottom:4px;margin-top:12px'>Pressure (hPa)</p>";
    h += "<svg viewBox='0 0 500 90' preserveAspectRatio='none' style='background:#0f0f1e;border-radius:8px'>";
    snprintf(buf, sizeof(buf), "<text x='4' y='13' font-size='10' fill='#555'>%.1f hPa</text><path d='", pmax);
    h += buf;
    {
      float alpha = (n > 1) ? 490.0f / (float)(n - 1) : 0.0f;
      auto ptX2 = [&](int k) -> float { return (n < 2) ? 250.0f : k * alpha + 5.0f; };
      auto ptY2 = [&](int k) -> float {
        int idx = n - 1 - k;
        return (pmax == pmin) ? 40.0f : 5.0f + (pmax - recs[idx].pressureHPa) / (pmax - pmin) * 65.0f;
      };
      snprintf(buf, sizeof(buf), "M%d,%.1f", (n < 2 ? 250 : 5), ptY2(0));
      h += buf;
      for (int k = 0; k < (int)n - 1; k++) {
        int km1 = (k > 0) ? k - 1 : 0;
        int k2  = (k + 2 < (int)n) ? k + 2 : n - 1;
        float cp1x = ptX2(k)     + (ptX2(k + 1) - ptX2(km1)) / 6.0f;
        float cp1y = ptY2(k)     + (ptY2(k + 1) - ptY2(km1)) / 6.0f;
        float cp2x = ptX2(k + 1) - (ptX2(k2)    - ptX2(k))   / 6.0f;
        float cp2y = ptY2(k + 1) - (ptY2(k2)    - ptY2(k))   / 6.0f;
        snprintf(buf, sizeof(buf), " C%.1f,%.1f %.1f,%.1f %.1f,%.1f",
                 cp1x, cp1y, cp2x, cp2y, ptX2(k + 1), ptY2(k + 1));
        h += buf;
      }
    }
    snprintf(buf, sizeof(buf),
             "' fill='none' stroke='#4a9ef5' stroke-width='2'/>"
             "<text x='4' y='72' font-size='10' fill='#555'>%.1f hPa</text>", pmin);
    h += buf;
    {
      char tl[14] = "-", tr[14] = "-";
      if (recs[n - 1].timestamp > 0) {
        time_t t = recs[n - 1].timestamp; struct tm ti; localtime_r(&t, &ti);
        strftime(tl, sizeof(tl), "%d.%m %H:%M", &ti);
      }
      if (recs[0].timestamp > 0) {
        time_t t = recs[0].timestamp; struct tm ti; localtime_r(&t, &ti);
        strftime(tr, sizeof(tr), "%d.%m %H:%M", &ti);
      }
      snprintf(buf, sizeof(buf),
               "<line x1='5' y1='76' x2='495' y2='76' stroke='#2a2a4a' stroke-width='1'/>"
               "<text x='5' y='87' font-size='9' fill='#666'>%s</text>"
               "<text x='495' y='87' font-size='9' fill='#666' text-anchor='end'>%s</text></svg>",
               tl, tr);
      h += buf;
    }
  }

  // ---- Records table ----
  h += "<table><tr><th>#</th><th>Time</th><th>Temp</th><th>Pressure</th><th>Alt</th></tr>";
  for (uint32_t i = 0; i < n; i++) {
    char tbuf[20] = "-";
    if (recs[i].timestamp > 0) {
      time_t t = recs[i].timestamp; struct tm ti; localtime_r(&t, &ti);
      strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M", &ti);
    }
    snprintf(buf, sizeof(buf),
             "<tr><td>%u</td><td>%s</td><td class='v'>%.1f&deg;C</td>"
             "<td>%.1f hPa / %.1f mmHg</td><td>%.1f m</td></tr>",
             tw - offset - i, tbuf, recs[i].temp,
             recs[i].pressureHPa, recs[i].pressureHPa / 1.33322f, recs[i].altitude);
    h += buf;
  }
  h += "</table>";

  // ---- Pagination navigation ----
  {
    bool hasNewer = (offset >= 50);
    bool hasOlder = (offset + 50 < tw);
    if (hasNewer || hasOlder) {
      h += "<div style='text-align:center;margin-top:12px'>";
      if (hasNewer) {
        snprintf(buf, sizeof(buf), "<a class='btn' href='/api/stats?offset=%u'>&larr; Newer</a>&nbsp;&nbsp;",
                 offset >= 50 ? offset - 50 : 0);
        h += buf;
      }
      if (hasOlder) {
        snprintf(buf, sizeof(buf), "<a class='btn' href='/api/stats?offset=%u'>Older &rarr;</a>",
                 offset + 50);
        h += buf;
      }
      h += "</div>";
    }
  }

  h += FPSTR(HTML_STATS_FOOT);
  request->send(200, "text/html", h);
}

// Pull-based state for the streamed CSV export. Held by a shared_ptr captured in
// the chunk filler so it lives exactly as long as the response.
struct ExportState {
  uint32_t count;
  uint32_t startIdx;
  uint32_t rec;        // next record index to emit (0..count)
  bool     headerSent;
  char     carry[96];  // one formatted CSV line awaiting copy into the TCP buffer
  size_t   carryLen;
  size_t   carryPos;
};

void handleExport(AsyncWebServerRequest *request) {
  if (!flashOK) {
    request->send(503, "text/plain", "Flash not available");
    return;
  }

  // Snapshot ring-buffer position under mutex.
  xSemaphoreTake(flashMutex, portMAX_DELAY);
  uint32_t count    = min(totalWritten, (uint32_t)MAX_RECORDS);
  uint32_t startIdx = (writeIdx - count + MAX_RECORDS) % MAX_RECORDS;
  xSemaphoreGive(flashMutex);

  auto state = std::make_shared<ExportState>();
  state->count      = count;
  state->startIdx   = startIdx;
  state->rec        = 0;
  state->headerSent = false;
  state->carryLen   = 0;
  state->carryPos   = 0;

  // Filler is called repeatedly by AsyncTCP; it emits the header, then one record
  // per flash read, copying into the buffer until it is full or all records are sent.
  AsyncWebServerResponse *res = request->beginChunkedResponse("text/csv",
    [state](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (state->carryPos >= state->carryLen) {     // need a fresh line
          state->carryPos = 0;
          state->carryLen = 0;
          if (!state->headerSent) {
            state->headerSent = true;
            int w = snprintf(state->carry, sizeof(state->carry),
                             "num,time,temperature_c,pressure_hpa,pressure_mmhg,altitude_m\r\n");
            state->carryLen = (w > 0) ? min((size_t)w, sizeof(state->carry) - 1) : 0;
          } else if (state->rec < state->count) {
            Record r;
            uint32_t idx = (state->startIdx + state->rec) % MAX_RECORDS;
            xSemaphoreTake(flashMutex, portMAX_DELAY);
            digitalWrite(WRITE_LED, HIGH);
            flash.readByteArray(RECORDS_ADDR + (uint32_t)idx * RECORD_SIZE, (uint8_t*)&r, RECORD_SIZE);
            digitalWrite(WRITE_LED, LOW);
            xSemaphoreGive(flashMutex);

            char tbuf[20] = "";
            if (r.timestamp > 0) {
              time_t t = r.timestamp; struct tm ti; localtime_r(&t, &ti);
              strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
            }
            int w = snprintf(state->carry, sizeof(state->carry),
                             "%lu,\"%s\",%.1f,%.1f,%.1f,%.1f\r\n",
                             (unsigned long)(state->rec + 1), tbuf,
                             r.temp, r.pressureHPa, r.pressureHPa / 1.33322f, r.altitude);
            state->carryLen = (w > 0) ? min((size_t)w, sizeof(state->carry) - 1) : 0;
            state->rec++;
          } else {
            break;  // all records emitted — returning <maxLen (incl. 0) ends the response
          }
        }
        while (state->carryPos < state->carryLen && written < maxLen)
          buffer[written++] = (uint8_t)state->carry[state->carryPos++];
      }
      return written;
    });

  res->addHeader("Content-Disposition", "attachment; filename=\"data.csv\"");
  request->send(res);
}

void handleProvision(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *res = request->beginResponse_P(200, "text/html", HTML_PROVISION);
  res->addHeader("Cache-Control", "no-cache");
  request->send(res);
}

// ===== Async WiFi scan state (polled by taskWeb; read by handlers) =====
static bool g_scanRunning = false;
static int  g_scanCount   = 0;

void startAsyncScan() {
  if (g_scanRunning) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(/*async=*/true);
  g_scanRunning = true;
}

void scanTick() {
  if (!g_scanRunning) return;
  int r = WiFi.scanComplete();
  if (r == WIFI_SCAN_RUNNING) return;
  g_scanCount   = (r >= 0) ? r : 0;
  g_scanRunning = false;
}

void handleScan(AsyncWebServerRequest *request) {
  int n = g_scanCount;
  // Static: AsyncWebServer dispatches handlers serially on one task, so no reentrancy.
  static char json[2048];
  uint16_t len = 0;
  json[len++] = '[';
  for (int i = 0; i < n; i++) {
    if (len > sizeof(json) - 100) break;
    if (i > 0) json[len++] = ',';

    // Copy SSID without String heap allocation
    char ssidRaw[33] = {};
    strncpy(ssidRaw, WiFi.SSID(i).c_str(), 32);

    // Full JSON string escaping: \, ", and control characters
    char ssidEsc[200] = {};
    uint8_t ei = 0;
    for (uint8_t j = 0; ssidRaw[j] && ei < sizeof(ssidEsc) - 7; j++) {
      unsigned char c = (unsigned char)ssidRaw[j];
      if      (c == '"')  { ssidEsc[ei++] = '\\'; ssidEsc[ei++] = '"';  }
      else if (c == '\\') { ssidEsc[ei++] = '\\'; ssidEsc[ei++] = '\\'; }
      else if (c == '\n') { ssidEsc[ei++] = '\\'; ssidEsc[ei++] = 'n';  }
      else if (c == '\r') { ssidEsc[ei++] = '\\'; ssidEsc[ei++] = 'r';  }
      else if (c == '\t') { ssidEsc[ei++] = '\\'; ssidEsc[ei++] = 't';  }
      else if (c < 0x20)  {
        ssidEsc[ei++] = '\\'; ssidEsc[ei++] = 'u';
        ssidEsc[ei++] = '0';  ssidEsc[ei++] = '0';
        ssidEsc[ei++] = "0123456789abcdef"[c >> 4];
        ssidEsc[ei++] = "0123456789abcdef"[c & 0xf];
      }
      else { ssidEsc[ei++] = c; }
    }

    bool enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    int w = snprintf(json + len, sizeof(json) - len,
                     "{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d}",
                     ssidEsc, WiFi.RSSI(i), enc ? 1 : 0);
    if (w > 0) len += (uint16_t)min(w, (int)(sizeof(json) - len - 1));
  }
  if (len >= (uint16_t)(sizeof(json) - 1)) len = sizeof(json) - 2;
  json[len++] = ']';
  json[len]   = '\0';

  startAsyncScan();
  AsyncWebServerResponse *res = request->beginResponse(200, "application/json", json);
  res->addHeader("Cache-Control", "no-cache");
  request->send(res);
}

void handleSave(AsyncWebServerRequest *request) {
  if (!request->hasParam("ssid", true)) {
    request->send(400, "text/plain", "Missing ssid");
    return;
  }
  char s[33] = {}, p[65] = {};
  strncpy(s, request->getParam("ssid", true)->value().c_str(), 32);
  if (request->hasParam("password", true))
    strncpy(p, request->getParam("password", true)->value().c_str(), 64);
  saveCredsToFlash(s, p);
  request->send(200, "text/plain", "OK");
  g_rebootAtMs = millis() + 800;  // restart after the response has flushed (done in loop())
}

void handleReboot(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "Rebooting...");
  g_rebootAtMs = millis() + 400;
}

void handleFlashReset(AsyncWebServerRequest *request) {
  flashReset();
  request->redirect("/api/stats");
}
