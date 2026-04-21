#pragma once
#include <WebServer.h>

extern WebServer server;

void handleRoot();
void handleApi();
void handleStats();
void handleFlashReset();
void handleExport();
void handleProvision();
void handleScan();
void handleSave();

// Async WiFi scan management — called from taskWeb
void startAsyncScan();
void scanTick();
