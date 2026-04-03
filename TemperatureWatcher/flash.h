#pragma once
#include <SPI.h>
#include <SPIMemory.h>
#include "config.h"

// ===== Shared sensor/credential globals (defined in .ino) =====
extern float temp;
extern float pressureHPa;
extern float altitude;
extern char ssid[];
extern char password[];

// ===== Structs =====

struct CredsRecord {
  uint32_t magic;
  char ssid[33];
  char password[65];
};

struct Record {        // 16 bytes
  uint32_t timestamp;  // Unix time (Kyiv)
  float temp;
  float pressureHPa;
  float altitude;
};

// ===== Flash state =====

SPIFlash flash(FLASH_CS);

uint32_t writeIdx = 0;
uint32_t totalWritten = 0;
uint16_t metaSlot = 0;  // current slot in sector 0 (0..META_SLOT_COUNT)
bool flashOK = false;

// ===== Functions =====

void flashWriteMeta() {
  if (metaSlot >= META_SLOT_COUNT) {
    flash.eraseSector(0);
    metaSlot = 0;
  }
  uint32_t addr = (uint32_t)metaSlot * META_SLOT_SIZE;
  flash.writeAnything(addr, writeIdx);
  flash.writeAnything(addr + 4, totalWritten);
  metaSlot++;
}

void flashInit() {
  if (!flash.begin()) {
    Serial.println("W25Q64 not found!");
    return;
  }
  flashOK = true;
  Serial.printf("Flash OK, capacity: %u bytes\n", flash.getCapacity());
  digitalWrite(WRITE_LED, HIGH);

  // Binary search for the first empty slot in sector 0.
  // Slots are written in order 0,1,2,…; empty = both words 0xFFFFFFFF.
  // log2(512) = 9 reads vs up to 1024 sequential reads.
  uint16_t lo = 0, hi = META_SLOT_COUNT;
  while (lo < hi) {
    uint16_t mid = lo + (hi - lo) / 2;
    uint32_t wi, tw;
    flash.readAnything((uint32_t)mid * META_SLOT_SIZE, wi);
    flash.readAnything((uint32_t)mid * META_SLOT_SIZE + 4, tw);
    if (wi == 0xFFFFFFFF && tw == 0xFFFFFFFF) hi = mid;
    else lo = mid + 1;
  }
  metaSlot = lo;

  if (lo == 0) {
    writeIdx = 0;
    totalWritten = 0;
    Serial.println("Flash: initialized fresh (capacity ~727 days at 2 min interval)");
  } else {
    uint32_t lastAddr = (uint32_t)(lo - 1) * META_SLOT_SIZE;
    flash.readAnything(lastAddr, writeIdx);
    flash.readAnything(lastAddr + 4, totalWritten);
    Serial.printf("Flash: resuming at record %u (total %u), meta slot %u/%u\n",
                  writeIdx, totalWritten, metaSlot, (uint16_t)META_SLOT_COUNT);
  }
  digitalWrite(WRITE_LED, LOW);
}

// Returns true and populates global ssid/password if magic matches.
bool readCredsFromFlash() {
  CredsRecord c;
  flash.readAnything(CREDS_ADDR, c);
  if (c.magic != CREDS_MAGIC) return false;
  c.ssid[32] = '\0';
  c.password[64] = '\0';
  strncpy(ssid, c.ssid, 32);
  ssid[32] = '\0';
  strncpy(password, c.password, 64);
  password[64] = '\0';
  return true;
}

// Erases the credentials sector and writes new credentials.
void saveCredsToFlash(const char* s, const char* p) {
  CredsRecord c;
  c.magic = CREDS_MAGIC;
  memset(c.ssid, 0, sizeof(c.ssid));
  memset(c.password, 0, sizeof(c.password));
  strncpy(c.ssid, s, 32);
  strncpy(c.password, p, 64);
  flash.eraseSector(CREDS_ADDR);
  flash.writeAnything(CREDS_ADDR, c);
}

void flashSaveRecord() {
  if (!flashOK) return;
  digitalWrite(WRITE_LED, HIGH);
  Record r = { (uint32_t)time(nullptr), temp, pressureHPa, altitude };
  uint32_t addr = RECORDS_ADDR + (uint32_t)writeIdx * RECORD_SIZE;
  if (writeIdx % 256 == 0) flash.eraseSector(addr);
  flash.writeAnything(addr, r);
  totalWritten++;
  writeIdx = (writeIdx + 1) % MAX_RECORDS;
  flashWriteMeta();
  digitalWrite(WRITE_LED, LOW);
}
