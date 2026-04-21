#pragma once
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

struct ScrollTrack {
  int pos          = 0;
  int dir          = 1;
  uint32_t pauseMs = 0;  // non-zero while paused at an end
};

extern LiquidCrystal_I2C lcd;   // 2004 (20×4) — sensor readings
extern LiquidCrystal_I2C lcd2;  // 1602 (16×2) — network info

extern char lcd2SsidVal[33];
extern char lcd2IpVal[16];
extern ScrollTrack lcd2SsidScroll;
extern ScrollTrack lcd2IpScroll;
extern uint32_t lcd2ScrollMs;

void lcdDrawLabels();
void lcd2ScrollTick(ScrollTrack& t, const char* val, uint8_t col, uint8_t row, int winSize);
const char* wifiStatusToString(int status);
