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

// Pressure trend — written by Core 1 (loop), read by Core 0 (handleApi).
// int8_t write is single-byte atomic on Xtensa LX6; volatile prevents register caching.
extern volatile int8_t pressureTrend;  // -1 falling, 0 stable, +1 rising

// 24-hour temperature min/max — written and read by Core 1 only (loop + handleStats via mutex).
extern float tempMin24h;
extern float tempMax24h;

// True while the BMP180 is returning valid readings.
extern volatile bool sensorOK;
