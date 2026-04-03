#pragma once
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===== LCD objects =====
// Main 2004 display (20×4) — sensor values
LiquidCrystal_I2C lcd(0x27, 20, 4);
// Secondary 1602 display (16×2) — network info / status
LiquidCrystal_I2C lcd2(0x26, 16, 2);

// ===== LCD2 scroll state =====
// Only the value portion scrolls; prefix labels ("WiFi: ", "IP: ") are drawn once.
char lcd2SsidVal[33] = {};  // SSID, up to 32 chars
char lcd2IpVal[16] = {};    // IP address, up to 15 chars

struct ScrollTrack {
  int pos = 0;
  int dir = 1;
  uint32_t pauseMs = 0;  // non-zero while paused at an end
};

ScrollTrack lcd2SsidScroll;
ScrollTrack lcd2IpScroll;
uint32_t lcd2ScrollMs = 0;

// ===== Functions =====

// Render one scrolling field on lcd2. Draws the visible window at (col, row) and
// advances the position, pausing 500 ms at each end before reversing.
// winSize — number of visible columns available for the value.
void lcd2ScrollTick(ScrollTrack& t, const char* val, uint8_t col, uint8_t row, int winSize) {
  int len = (int)strlen(val);
  if (len <= winSize) return;  // fits — nothing to scroll

  char win[17];
  strncpy(win, val + t.pos, winSize);
  win[winSize] = '\0';
  lcd2.setCursor(col, row);
  lcd2.print(win);

  int maxPos = len - winSize;
  if (t.pauseMs == 0) {
    t.pos += t.dir;
    if (t.pos >= maxPos) {
      t.pos = maxPos;
      t.dir = -1;
      t.pauseMs = millis();
    }
    if (t.pos <= 0) {
      t.pos = 0;
      t.dir = 1;
      t.pauseMs = millis();
    }
  } else if (millis() - t.pauseMs >= 500) {
    t.pauseMs = 0;
  }
}

// Draw static row labels once — never redrawn to avoid flicker.
void lcdDrawLabels() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Temp:");
  lcd.setCursor(0, 1);
  lcd.print("hPa:");
  lcd.setCursor(0, 2);
  lcd.print("mmHg:");
  lcd.setCursor(0, 3);
  lcd.print("Alt:");
}
