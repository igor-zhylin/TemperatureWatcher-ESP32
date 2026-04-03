#pragma once

// ===== Pin assignments =====
#define LED_PIN     2    // Built-in LED (GPIO 2)
#define WRITE_LED   13   // Red LED: GPIO13 → 220Ω → LED → GND

// ===== W25Q64 SPI Flash =====
// Wiring: MOSI=23, MISO=19, SCK=18, CS=5 (default ESP32 VSPI)
#define FLASH_CS         5
#define RECORDS_ADDR     4096UL     // Records start at sector 1 (sector 0 = metadata)
#define MAX_RECORDS      523776     // Sectors 1–2046 × 256 records/sector; sector 2047 = credentials
#define CREDS_ADDR       8384512UL  // Last sector of W25Q64: sector 2047 × 4096 bytes
#define RECORD_SIZE      16         // sizeof(Record) — must stay 16
#define CREDS_MAGIC      0xC0FFEE01u

// Wear leveling for metadata sector (sector 0, 4096 bytes):
// 512 slots × 8 bytes — write appends to next slot, erase only when sector is full.
// Reduces erase frequency 512× (every ~17 h instead of every 2 min → ~190 years).
#define META_SLOT_SIZE   8
#define META_SLOT_COUNT  (4096 / META_SLOT_SIZE)  // 512 slots per erase cycle

// ===== Timing =====
#define SAVE_INTERVAL        120000UL  // Flash write interval: 2 min → 523776 records ≈ 727 days
#define WIFI_RETRY_INTERVAL  120000UL  // Delay between WiFi reconnect attempts
