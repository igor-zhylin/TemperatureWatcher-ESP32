#include "tasks.h"
#include <DNSServer.h>
#include "globals.h"
#include "web_handlers.h"

extern DNSServer dnsServer;

void taskWeb(void* pv) {
  for (;;) {
    if (apMode) dnsServer.processNextRequest();
    server.handleClient();
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}
