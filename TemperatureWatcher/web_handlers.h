#pragma once
#include <ESPAsyncWebServer.h>

extern AsyncWebServer server;

// Set to a millis() deadline by handlers that must restart the device; loop()
// performs the actual ESP.restart() once the response has had time to flush.
extern volatile uint32_t g_rebootAtMs;

void handleRoot(AsyncWebServerRequest *request);
void handleApi(AsyncWebServerRequest *request);
void handleStats(AsyncWebServerRequest *request);
void handleFlashReset(AsyncWebServerRequest *request);
void handleExport(AsyncWebServerRequest *request);
void handleProvision(AsyncWebServerRequest *request);
void handleReboot(AsyncWebServerRequest *request);
void handleScan(AsyncWebServerRequest *request);
void handleSave(AsyncWebServerRequest *request);

// Async WiFi scan management — polled from taskWeb
void startAsyncScan();
void scanTick();
