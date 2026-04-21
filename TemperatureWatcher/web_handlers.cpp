#include "web_handlers.h"
#include <WiFi.h>
#include "flash.h"
#include "globals.h"
#include "html_pages.h"
#include "html_stats.h"
#include <time.h>

WebServer server(80);

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", HTML_ROOT, sizeof(HTML_ROOT));
}

void handleApi() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  char json[128];
  snprintf(json, sizeof(json),
           "{\"temperature_c\":%.1f,\"pressure_hpa\":%.1f,\"pressure_mmhg\":%.1f,\"altitude_m\":%.1f}",
           (float)temp, (float)pressureHPa, (float)pressureMmHg, (float)altitude);
  server.send(200, "application/json", json);
}

void handleStats() {
  if (!flashOK) {
    server.send(503, "text/plain", "Flash not available");
    return;
  }

  // Snapshot ring-buffer position under mutex so n and startIdx are consistent.
  xSemaphoreTake(flashMutex, portMAX_DELAY);
  uint32_t n        = min((uint32_t)50, totalWritten);
  uint32_t startIdx = (writeIdx - n + MAX_RECORDS) % MAX_RECORDS;

  if (n == 0) {
    xSemaphoreGive(flashMutex);
    server.send(200, "text/html",
                "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;"
                "justify-content:center;align-items:center;min-height:100vh;margin:0}"
                "a{color:#e94560}</style></head>"
                "<body><p>No records yet. <a href='/'>Back</a></p></body></html>");
    return;
  }

  // Bulk flash read — wrap-around aware.
  Record recs[50];
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

  // Reverse in place (newest first) — works on local RAM copy, no mutex needed.
  for (uint32_t i = 0, j = n - 1; i < j; i++, j--) {
    Record t = recs[i]; recs[i] = recs[j]; recs[j] = t;
  }

  float tmin = recs[0].temp, tmax = recs[0].temp;
  for (uint32_t i = 1; i < n; i++) {
    if (recs[i].temp < tmin) tmin = recs[i].temp;
    if (recs[i].temp > tmax) tmax = recs[i].temp;
  }

  WiFiClient client = server.client();
  client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"));

  char outbuf[2048];
  uint16_t outlen = 0;
  auto flush = [&]() {
    if (outlen > 0) { client.write((const uint8_t*)outbuf, outlen); outlen = 0; yield(); }
  };
  auto ap = [&](const char* s) {
    uint16_t l = (uint16_t)strlen(s);
    if (l >= sizeof(outbuf)) { flush(); client.write((const uint8_t*)s, l); yield(); return; }
    if (outlen + l >= sizeof(outbuf)) flush();
    memcpy(outbuf + outlen, s, l);
    outlen += l;
  };

  ap(HTML_STATS_HEAD);
  flush();

  char buf[160];
  snprintf(buf, sizeof(buf),
           "<h1>Temperature History</h1>"
           "<div class='sub'>Stored: %u | Showing: %u records</div>",
           totalWritten, n);
  ap(buf);

  ap("<svg viewBox='0 0 500 90' preserveAspectRatio='none' style='background:#0f0f1e;border-radius:8px'>");
  snprintf(buf, sizeof(buf), "<text x='4' y='13' font-size='10' fill='#555'>%.1f C</text><path d='", tmax);
  ap(buf);
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
      int km1 = (k > 0) ? k - 1 : 0;
      int k2  = (k + 2 < (int)n) ? k + 2 : n - 1;
      float cp1x = ptX(k)     + (ptX(k + 1) - ptX(km1)) / 6.0f;
      float cp1y = ptY(k)     + (ptY(k + 1) - ptY(km1)) / 6.0f;
      float cp2x = ptX(k + 1) - (ptX(k2)    - ptX(k))   / 6.0f;
      float cp2y = ptY(k + 1) - (ptY(k2)    - ptY(k))   / 6.0f;
      snprintf(buf, sizeof(buf), " C%.1f,%.1f %.1f,%.1f %.1f,%.1f",
               cp1x, cp1y, cp2x, cp2y, ptX(k + 1), ptY(k + 1));
      ap(buf);
    }
  }
  snprintf(buf, sizeof(buf),
           "' fill='none' stroke='#e94560' stroke-width='2'/>"
           "<text x='4' y='72' font-size='10' fill='#555'>%.1f C</text>",
           tmin);
  ap(buf);
  ap("<line x1='5' y1='76' x2='495' y2='76' stroke='#2a2a4a' stroke-width='1'/>");
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
    ap(buf);
  }
  flush();

  ap("<table><tr><th>#</th><th>Time</th><th>Temp</th><th>Pressure</th><th>Alt</th></tr>");
  for (uint32_t i = 0; i < n; i++) {
    char tbuf[20] = "-";
    if (recs[i].timestamp > 0) {
      time_t t = recs[i].timestamp; struct tm ti; localtime_r(&t, &ti);
      strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M", &ti);
    }
    snprintf(buf, sizeof(buf),
             "<tr><td>%u</td><td>%s</td><td class='v'>%.1f&deg;C</td>"
             "<td>%.1f hPa / %.1f mmHg</td><td>%.1f m</td></tr>",
             totalWritten - i, tbuf, recs[i].temp,
             recs[i].pressureHPa, recs[i].pressureHPa / 1.33322f, recs[i].altitude);
    ap(buf);
  }

  ap(HTML_STATS_FOOT);
  flush();
  client.stop();
}

void handleFlashReset() {
  if (!flashOK) {
    server.send(503, "text/plain", "Flash not available");
    return;
  }
  flashReset();
  server.sendHeader("Location", "/api/stats");
  server.send(303);
}

void handleExport() {
  if (!flashOK) {
    server.send(503, "text/plain", "Flash not available");
    return;
  }

  // Snapshot ring-buffer position under mutex.
  xSemaphoreTake(flashMutex, portMAX_DELAY);
  uint32_t count    = min(totalWritten, (uint32_t)MAX_RECORDS);
  uint32_t startIdx = (writeIdx - count + MAX_RECORDS) % MAX_RECORDS;
  xSemaphoreGive(flashMutex);

  WiFiClient client = server.client();
  client.print(F("HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/csv\r\n"
                 "Content-Disposition: attachment; filename=\"data.csv\"\r\n"
                 "Connection: close\r\n\r\n"
                 "num,time,temperature_c,pressure_hpa,pressure_mmhg,altitude_m\r\n"));

  char outbuf[1024];
  uint16_t outlen = 0;
  Record batch[16];

  for (uint32_t i = 0; i < count; i += 16) {
    uint32_t batchSize  = min((uint32_t)16, count - i);
    uint32_t batchStart = (startIdx + i) % MAX_RECORDS;

    // Take mutex per batch so flashSaveRecord() can interleave between batches.
    xSemaphoreTake(flashMutex, portMAX_DELAY);
    digitalWrite(WRITE_LED, HIGH);
    if (batchStart + batchSize <= MAX_RECORDS) {
      flash.readByteArray(RECORDS_ADDR + batchStart * RECORD_SIZE,
                          (uint8_t*)batch, batchSize * RECORD_SIZE);
    } else {
      uint32_t fp = MAX_RECORDS - batchStart;
      flash.readByteArray(RECORDS_ADDR + batchStart * RECORD_SIZE, (uint8_t*)batch, fp * RECORD_SIZE);
      flash.readByteArray(RECORDS_ADDR, (uint8_t*)batch + fp * RECORD_SIZE, (batchSize - fp) * RECORD_SIZE);
    }
    digitalWrite(WRITE_LED, LOW);
    xSemaphoreGive(flashMutex);

    for (uint32_t j = 0; j < batchSize; j++) {
      char tbuf[20] = "";
      if (batch[j].timestamp > 0) {
        time_t t = batch[j].timestamp; struct tm ti; localtime_r(&t, &ti);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
      }
      if (outlen + 80 > sizeof(outbuf)) {
        client.write((const uint8_t*)outbuf, outlen);
        outlen = 0;
        yield();
      }
      outlen += snprintf(outbuf + outlen, sizeof(outbuf) - outlen,
                         "%lu,\"%s\",%.1f,%.1f,%.1f,%.1f\r\n",
                         (unsigned long)(i + j + 1), tbuf,
                         batch[j].temp, batch[j].pressureHPa,
                         batch[j].pressureHPa / 1.33322f, batch[j].altitude);
    }
  }
  if (outlen > 0) client.write((const uint8_t*)outbuf, outlen);
  client.stop();
}

void handleProvision() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", HTML_PROVISION);
}

void handleScan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(/*async=*/true);
  unsigned long start = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    if (millis() - start > 6000) break;
    delay(50);  // yields to TCP/IP FreeRTOS task; Core 1 sensor loop runs normally
  }
  int n = WiFi.scanComplete();
  if (n < 0) n = 0;

  char json[4200];
  uint16_t len = 0;
  json[len++] = '[';
  for (int i = 0; i < n; i++) {
    if (len > sizeof(json) - 80) break;
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
