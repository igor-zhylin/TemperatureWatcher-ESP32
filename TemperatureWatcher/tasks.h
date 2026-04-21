#pragma once

// Web server task — runs on Core 0.
// Spawned in setup(); handles all HTTP requests and captive-portal DNS.
// Core 1 (Arduino loop) owns sensors, LCD, flash saves, and WiFi reconnect.
void taskWeb(void* pvParameters);
