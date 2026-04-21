#pragma once

// Sensor readings — written by Core 1 (loop), read by Core 0 (taskWeb/handleApi).
// 4-byte aligned floats on Xtensa LX6 are written atomically; volatile prevents register caching.
extern volatile float temp;
extern volatile float pressureHPa;
extern volatile float pressureMmHg;
extern volatile float altitude;

// WiFi credentials — written once at boot/save, read-only afterwards.
extern char ssid[33];
extern char password[65];

// AP mode flag — written by Core 1 setup, read by Core 0 taskWeb.
extern volatile bool apMode;
