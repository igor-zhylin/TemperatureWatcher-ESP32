#include "tasks.h"
#include <DNSServer.h>
#include "globals.h"
#include "web_handlers.h"

extern DNSServer dnsServer;

void taskWeb(void* pv) {
  startAsyncScan();  // prime the cache before the first /api/scan request arrives
  for (;;) {
    scanTick();  // advance async WiFi scan without blocking
    if (apMode) dnsServer.processNextRequest();
    server.handleClient();
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}
