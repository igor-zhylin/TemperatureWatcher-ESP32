#pragma once
#include <SPI.h>
#include <SPIMemory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

struct CredsRecord {
  uint32_t magic;
  char ssid[33];
  char password[65];
};

struct Record {       // 16 bytes
  uint32_t timestamp; // Unix time (Kyiv)
  float temp;
  float pressureHPa;
  float altitude;
};

extern SPIFlash flash;
extern uint32_t writeIdx;
extern uint32_t totalWritten;
extern uint16_t metaSlot;
extern bool flashOK;

void flashInit();
bool readCredsFromFlash();
bool saveCredsToFlash(const char* s, const char* p);
void flashSaveRecord();
bool flashReset();

// ===== Bounded-wait flash access for web handlers (the single AsyncTCP task) =====
// SPIMemory's internal busy-poll (SPIFlashIO.cpp's _notBusy(), no yield, ~1000s
// default timeout) can block whichever task calls it if an SPI glitch/brownout
// leaves the W25Q64's BUSY status bit stuck. flashSaveRecord() above still calls
// the library directly from loop() (Core 1) — that's fine, since loop() is the
// only task the hardware watchdog watches, so a genuine stuck chip there still
// self-recovers via a panic reboot. Anything reachable from an HTTP handler,
// however, runs on the single AsyncTCP task that serves every browser tab, so a
// stuck chip there would freeze the whole web UI (incl. switching tabs) for as
// long as the busy-poll runs. These wrappers run the actual flash op on a
// dedicated low-priority worker task instead and wait with a short bounded
// timeout, so a stuck chip can only ever block that worker — never the shared
// AsyncTCP task — and the caller just gets a clean failure to turn into a 503.
bool flashSafeReadBytes(uint32_t addr, uint8_t* buf, size_t len, uint32_t timeoutMs = 2000);
bool flashSafeSnapshot(uint32_t* outWriteIdx, uint32_t* outTotalWritten, uint32_t timeoutMs = 200);
