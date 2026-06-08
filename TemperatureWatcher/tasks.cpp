#include "tasks.h"
#include <DNSServer.h>
#include "globals.h"
#include "web_handlers.h"

extern DNSServer dnsServer;

// ESPAsyncWebServer is event-driven (served by the AsyncTCP task), so there is no
// handleClient() to pump. This task only advances the async WiFi scan and the
// captive-portal DNS while in AP mode.
void taskWeb(void* pv) {
  startAsyncScan();  // prime the cache before the first /api/scan request arrives
  for (;;) {
    scanTick();  // advance async WiFi scan without blocking
    if (apMode) dnsServer.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
