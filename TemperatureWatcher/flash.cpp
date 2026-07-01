#include "flash.h"
#include "globals.h"
#include "config.h"
#include <Arduino.h>
#include <time.h>

SPIFlash flash(FLASH_CS);
uint32_t writeIdx     = 0;
uint32_t totalWritten = 0;
uint16_t metaSlot     = 0;
bool flashOK          = false;
static SemaphoreHandle_t flashMutex = nullptr;

static void flashWriteMeta() {
  if (metaSlot >= META_SLOT_COUNT) {
    flash.eraseSector(0);
    metaSlot = 0;
  }
  uint32_t addr = (uint32_t)metaSlot * META_SLOT_SIZE;
  // Single writeAnything call = single SPI page-program command = atomic.
  // Two separate calls would leave totalWritten=0xFFFFFFFF on power loss between them.
  struct { uint32_t wi; uint32_t tw; } m = { writeIdx, totalWritten };
  flash.writeAnything(addr, m);
  metaSlot++;
}

// ===== Flash worker task =====
// Executes every flash op reachable from an HTTP handler (the single AsyncTCP
// task) on a dedicated low-priority task instead, so a chip stuck in SPIMemory's
// unbounded busy-poll (_notBusy() in SPIFlashIO.cpp: no yield, ~1000s default
// timeout) can only ever block this worker -- never the web server. FreeRTOS is
// fully preemptive and this task's priority (1) is well below AsyncTCP's
// (CONFIG_ASYNC_TCP_PRIORITY, default 10), so the web server task always
// preempts a spinning worker the instant it has a request to service.
enum class FlashOp : uint8_t { Read, SaveCreds, ResetFlash };

static TaskHandle_t      flashWorkerHandle = nullptr;
static SemaphoreHandle_t flashReqMutex     = nullptr;  // serializes safe-* callers
static SemaphoreHandle_t flashReqSem       = nullptr;  // caller -> worker: request pending
static SemaphoreHandle_t flashDoneSem      = nullptr;  // worker -> caller: request complete
static volatile bool     flashWorkerBusy   = false;    // true while a request is outstanding

static struct {
  FlashOp  op;
  uint32_t addr;
  uint8_t* buf;              // Read: destination buffer
  size_t   len;               // Read: byte count
  char     credSsid[33];      // SaveCreds
  char     credPassword[65];  // SaveCreds
  bool     ok;
} flashReq;

static void flashWorkerTask(void*) {
  for (;;) {
    xSemaphoreTake(flashReqSem, portMAX_DELAY);
    xSemaphoreTake(flashMutex, portMAX_DELAY);
    digitalWrite(WRITE_LED, HIGH);
    switch (flashReq.op) {
      case FlashOp::Read:
        flashReq.ok = flash.readByteArray(flashReq.addr, flashReq.buf, flashReq.len);
        break;
      case FlashOp::SaveCreds: {
        CredsRecord c;
        c.magic = CREDS_MAGIC;
        memset(c.ssid, 0, sizeof(c.ssid));
        memset(c.password, 0, sizeof(c.password));
        strncpy(c.ssid, flashReq.credSsid, 32);
        strncpy(c.password, flashReq.credPassword, 64);
        flashReq.ok = flash.eraseSector(CREDS_ADDR) && flash.writeAnything(CREDS_ADDR, c);
        break;
      }
      case FlashOp::ResetFlash:
        flash.eraseSector(0);
        writeIdx     = 0;
        totalWritten = 0;
        metaSlot     = 0;
        flashWriteMeta();
        flashReq.ok = true;
        break;
    }
    digitalWrite(WRITE_LED, LOW);
    xSemaphoreGive(flashMutex);
    xSemaphoreGive(flashDoneSem);
  }
}

static void flashWorkerStart() {
  flashReqMutex = xSemaphoreCreateMutex();
  flashReqSem   = xSemaphoreCreateBinary();
  flashDoneSem  = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(flashWorkerTask, "FlashWorker", 4096, nullptr, 1, &flashWorkerHandle, 0);
}

// Posts the already-populated flashReq to the worker and waits up to timeoutMs
// for it to finish. Caller must hold flashReqMutex. If a previous request never
// confirmed completion (worker still stuck, or its late completion just hasn't
// been drained yet), refuses rather than race the shared flashReq slot.
static bool flashPostAndWait(uint32_t timeoutMs) {
  if (flashWorkerBusy && xSemaphoreTake(flashDoneSem, 0) == pdTRUE) {
    flashWorkerBusy = false;
  }
  if (flashWorkerBusy) return false;

  flashReq.ok     = false;
  flashWorkerBusy = true;
  xSemaphoreGive(flashReqSem);
  if (xSemaphoreTake(flashDoneSem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
    flashWorkerBusy = false;
    return flashReq.ok;
  }
  Serial.println("Flash worker timed out -- possible stuck/glitched SPI flash chip");
  return false;
}

bool flashSafeReadBytes(uint32_t addr, uint8_t* buf, size_t len, uint32_t timeoutMs) {
  if (!flashOK || !flashWorkerHandle) return false;
  if (xSemaphoreTake(flashReqMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  flashReq.op   = FlashOp::Read;
  flashReq.addr = addr;
  flashReq.buf  = buf;
  flashReq.len  = len;
  bool ok = flashPostAndWait(timeoutMs);
  xSemaphoreGive(flashReqMutex);
  return ok;
}

bool flashSafeSnapshot(uint32_t* outWriteIdx, uint32_t* outTotalWritten, uint32_t timeoutMs) {
  if (xSemaphoreTake(flashMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  *outWriteIdx     = writeIdx;
  *outTotalWritten = totalWritten;
  xSemaphoreGive(flashMutex);
  return true;
}

void flashInit() {
  flashMutex = xSemaphoreCreateMutex();
  if (!flash.begin()) {
    Serial.println("W25Q64 not found!");
    return;
  }
  flashOK = true;
  Serial.printf("Flash OK, capacity: %u bytes\n", flash.getCapacity());
  digitalWrite(WRITE_LED, HIGH);

  // Binary search for the first empty slot in sector 0.
  // Sorted layout: [valid ... valid | empty ... empty] — 9 SPI reads (log2 512).
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
    Serial.println("Flash: initialized fresh (~727 days at 2 min interval)");
  } else {
    uint32_t lastAddr = (uint32_t)(lo - 1) * META_SLOT_SIZE;
    struct { uint32_t wi; uint32_t tw; } m;
    flash.readAnything(lastAddr, m);
    writeIdx     = m.wi;
    totalWritten = m.tw;
    Serial.printf("Flash: resuming at record %u (total %u), meta slot %u/%u\n",
                  writeIdx, totalWritten, metaSlot, (uint16_t)META_SLOT_COUNT);
  }
  digitalWrite(WRITE_LED, LOW);

  flashWorkerStart();
}

bool readCredsFromFlash() {
  CredsRecord c;
  flash.readAnything(CREDS_ADDR, c);
  if (c.magic != CREDS_MAGIC) return false;
  c.ssid[32] = '\0';
  c.password[64] = '\0';
  strncpy(ssid, c.ssid, 32);     ssid[32] = '\0';
  strncpy(password, c.password, 64); password[64] = '\0';
  return true;
}

bool saveCredsToFlash(const char* s, const char* p) {
  if (!flashOK || !flashWorkerHandle) return false;
  if (xSemaphoreTake(flashReqMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  flashReq.op = FlashOp::SaveCreds;
  strncpy(flashReq.credSsid, s, 32);     flashReq.credSsid[32]     = '\0';
  strncpy(flashReq.credPassword, p, 64); flashReq.credPassword[64] = '\0';
  bool ok = flashPostAndWait(2000);
  xSemaphoreGive(flashReqMutex);
  return ok;
}

void flashSaveRecord() {
  if (!flashOK) return;
  xSemaphoreTake(flashMutex, portMAX_DELAY);
  digitalWrite(WRITE_LED, HIGH);
  Record r = { (uint32_t)time(nullptr), (float)temp, (float)pressureHPa, (float)altitude };
  uint32_t addr = RECORDS_ADDR + (uint32_t)writeIdx * RECORD_SIZE;
  if (writeIdx % 256 == 0) flash.eraseSector(addr);
  flash.writeAnything(addr, r);
  totalWritten++;
  writeIdx = (writeIdx + 1) % MAX_RECORDS;
  flashWriteMeta();
  digitalWrite(WRITE_LED, LOW);
  xSemaphoreGive(flashMutex);
}

bool flashReset() {
  if (!flashOK || !flashWorkerHandle) return false;
  if (xSemaphoreTake(flashReqMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  flashReq.op = FlashOp::ResetFlash;
  bool ok = flashPostAndWait(2000);
  xSemaphoreGive(flashReqMutex);
  if (ok) Serial.println("Flash reset by user");
  return ok;
}
