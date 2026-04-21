#include "lcd_display.h"
#include <Arduino.h>
#include <WiFi.h>

LiquidCrystal_I2C lcd(0x27, 20, 4);
LiquidCrystal_I2C lcd2(0x26, 16, 2);
char lcd2SsidVal[33]    = {};
char lcd2IpVal[16]      = {};
ScrollTrack lcd2SsidScroll;
ScrollTrack lcd2IpScroll;
uint32_t lcd2ScrollMs   = 0;

const char* wifiStatusToString(int status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "Idle";
    case WL_NO_SSID_AVAIL:   return "SSID not found!";
    case WL_SCAN_COMPLETED:  return "Scan done";
    case WL_CONNECTED:       return "Connected!";
    case WL_CONNECT_FAILED:  return "Wrong password!";
    case WL_CONNECTION_LOST: return "Connection lost";
    case WL_DISCONNECTED:    return "Disconnected";
    default:                 return "Unknown";
  }
}

void lcd2ScrollTick(ScrollTrack& t, const char* val, uint8_t col, uint8_t row, int winSize) {
  int len = (int)strlen(val);
  if (len <= winSize) return;

  char win[17];
  strncpy(win, val + t.pos, winSize);
  win[winSize] = '\0';
  lcd2.setCursor(col, row);
  lcd2.print(win);

  int maxPos = len - winSize;
  if (t.pauseMs == 0) {
    t.pos += t.dir;
    if (t.pos >= maxPos) { t.pos = maxPos; t.dir = -1; t.pauseMs = millis(); }
    if (t.pos <= 0)      { t.pos = 0;      t.dir =  1; t.pauseMs = millis(); }
  } else if (millis() - t.pauseMs >= 500) {
    t.pauseMs = 0;
  }
}

void lcdDrawLabels() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Temp:");
  lcd.setCursor(0, 1); lcd.print("hPa:");
  lcd.setCursor(0, 2); lcd.print("mmHg:");
  lcd.setCursor(0, 3); lcd.print("Alt:");
}
