# Wiring Reference — TemperatureWatcher ESP32

## I2C Bus (GPIO 21 SDA / GPIO 22 SCL)

All three devices share the same I2C bus. Power supply is 3.3 V or 5 V depending on the module.

| Device | I2C Address | Size | Purpose |
|---|---|---|---|
| LCD 2004 (20×4) | `0x27` | 20 columns, 4 rows | Sensor readings: temperature, pressure (hPa/mmHg), altitude |
| LCD 1602 (16×2) | `0x26` | 16 columns, 2 rows | Network info: row 0 — `WiFi: <SSID>`, row 1 — `IP: <address>` |
| BMP180 | `0x77` (hardware-fixed) | — | Temperature, atmospheric pressure, altitude sensor |

> The LCD 1602 has the A0 address jumper soldered, shifting the address from `0x27` to `0x26` to avoid conflict with the LCD 2004.

### I2C connections to ESP32

```
ESP32 GPIO 21 (SDA) ──┬── BMP180 SDA
                      ├── LCD 2004 adapter SDA
                      └── LCD 1602 adapter SDA

ESP32 GPIO 22 (SCL) ──┬── BMP180 SCL
                      ├── LCD 2004 adapter SCL
                      └── LCD 1602 adapter SCL
```

---

## SPI Bus — W25Q64 (8 MB Flash)

Uses VSPI (ESP32 hardware SPI).

| Signal | ESP32 GPIO | W25Q64 pin |
|---|---|---|
| MOSI | GPIO 23 | DI |
| MISO | GPIO 19 | DO |
| SCK | GPIO 18 | CLK |
| CS (Chip Select) | GPIO 5 | /CS |
| VCC | 3.3 V | VCC |
| GND | GND | GND |

---

## GPIO — Discrete Components

| Component | ESP32 GPIO | Wiring | Behavior |
|---|---|---|---|
| Built-in LED | GPIO 2 | on-board | Blinks every 500 ms on each sensor read |
| Write LED (red) | GPIO 13 | GPIO 13 → 220 Ω → LED → GND | On while writing a record to Flash |
| Flash reset button | GPIO 4 | GPIO 4 → button → GND (INPUT_PULLUP) | Hold 2 s to erase all Flash records |

---

## Full ESP32 Pin Allocation

| GPIO | Function | Bus / Mode |
|---|---|---|
| 2 | Built-in LED | OUTPUT |
| 4 | Flash reset button | INPUT_PULLUP |
| 5 | SPI CS — W25Q64 | VSPI |
| 13 | Write indicator LED | OUTPUT |
| 18 | SPI SCK — W25Q64 | VSPI |
| 19 | SPI MISO — W25Q64 | VSPI |
| 21 | I2C SDA | I2C |
| 22 | I2C SCL | I2C |
| 23 | SPI MOSI — W25Q64 | VSPI |

---

## Power Supply

| Component | Voltage |
|---|---|
| ESP32 | 5 V (USB) or 3.3 V |
| BMP180 | 3.3 V |
| W25Q64 | 3.3 V |
| LCD 2004 / LCD 1602 | 5 V (brighter backlight) or 3.3 V |

---

## Block Diagram (ASCII)

```
                        ┌─────────────────────────────┐
                        │           ESP32              │
                        │                              │
    ┌──────────┐        │  GPIO 21 (SDA) ──────────────┼──┬── BMP180
    │ LCD 2004 │◄───────┤  GPIO 22 (SCL) ──────────────┼──┤   (I2C 0x77)
    │  (0x27)  │        │                              │  ├── LCD 2004 (0x27)
    └──────────┘        │                              │  └── LCD 1602 (0x26)
    ┌──────────┐        │                              │
    │ LCD 1602 │◄───────┤  GPIO 23 (MOSI) ─────────────┼──┐
    │  (0x26)  │        │  GPIO 19 (MISO) ─────────────┼──┤  W25Q64
    └──────────┘        │  GPIO 18 (SCK)  ─────────────┼──┤  (SPI Flash)
                        │  GPIO  5 (CS)   ─────────────┼──┘
                        │                              │
                        │  GPIO  2 ────────────────────┼── Built-in LED
                        │  GPIO 13 ────────────────────┼── 220 Ω ── Red LED ── GND
                        │  GPIO  4 ────────────────────┼── Button ── GND
                        └─────────────────────────────┘
```
