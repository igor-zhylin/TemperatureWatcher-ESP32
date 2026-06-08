#pragma once

// ===== Pin assignments =====
#define LED_PIN     2    // Built-in LED (GPIO 2)
#define WRITE_LED   13   // Red LED: GPIO13 → 220Ω → LED → GND

// ===== W25Q64 SPI Flash =====
// Wiring: MOSI=23, MISO=19, SCK=18, CS=5 (default ESP32 VSPI)
#define FLASH_CS 5
#define RECORDS_ADDR 4096UL   // Records start at sector 1 (sector 0 = metadata)
#define MAX_RECORDS 523776    // Sectors 1–2046 × 256 records/sector; sector 2047 = credentials
#define CREDS_ADDR 8384512UL  // Last sector of W25Q64: sector 2047 × 4096 bytes
#define RECORD_SIZE 16        // sizeof(Record) — must stay 16
#define CREDS_MAGIC 0xC0FFEE01u

// Wear leveling for metadata sector (sector 0, 4096 bytes):
// 512 slots × 8 bytes — write appends to next slot, erase only when sector is full.
// Reduces erase frequency 512× (every ~17 h instead of every 2 min → ~190 years).
#define META_SLOT_SIZE 8
#define META_SLOT_COUNT (4096 / META_SLOT_SIZE)  // 512 slots per erase cycle

// ===== Timing =====
#define SAVE_INTERVAL 120000UL        // Flash write interval: 2 min → 523776 records ≈ 727 days
#define WIFI_RETRY_INTERVAL    30000UL   // Delay between reconnect attempts when connection is lost (30 s)
#define WIFI_PERIODIC_INTERVAL 300000UL  // Proactive reconnect interval (5 min) to refresh IP and LCD2
// After this many soft retries a full radio reset is triggered (WiFi_OFF → WiFi_STA → begin).
// Covers the ESP32 driver hang that makes soft reconnects silently fail.
// 10 retries × 30 s = ~5 min before escalation; keeps trying indefinitely after that.
#define WIFI_HARD_RESET_AFTER  10
