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
extern SemaphoreHandle_t flashMutex;

void flashInit();
bool readCredsFromFlash();
void saveCredsToFlash(const char* s, const char* p);
void flashSaveRecord();
void flashReset();
