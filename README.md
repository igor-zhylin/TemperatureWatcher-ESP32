# TemperatureWatcher-ESP32

ESP32-based weather station that reads temperature, atmospheric pressure, and altitude from a BMP180 sensor, logs every reading to an 8 MB SPI flash chip, and serves live data and history over Wi-Fi through a built-in web server.

---

## Features

- **BMP180 sensor** — temperature (°C), pressure (hPa / mmHg), altitude (m)
- **Two I2C LCD displays**
  - LCD 2004 (20×4) — live sensor readings, updated every 500 ms
  - LCD 1602 (16×2) — Wi-Fi SSID and IP address; scrolls long values
- **Web interface** — live readings page + history page with temperature chart + CSV export
- **8 MB SPI flash logging** — record saved every 2 minutes, capacity ~727 days
- **Flash wear leveling** — metadata sector endurance extended ~190 years (see below)
- **Physical reset button** — hold 2 s on GPIO 4 to erase all flash records
- **Wi-Fi reconnect** — if connection drops, retries every 2 minutes automatically; lcd2 shows retry counter and error code

---

## Hardware

| Component | Notes |
|---|---|
| ESP32 (any 38-pin board) | Tested on standard DevKit v1 |
| BMP180 breakout | Compatible with Adafruit BMP085 library |
| LCD 2004 with I2C backpack | Address `0x27` |
| LCD 1602 with I2C backpack | Address `0x26` — A0 jumper must be soldered |
| W25Q64 SPI flash (8 MB) | VSPI bus, CS on GPIO 5 |
| Red LED + 220 Ω resistor | Write activity indicator, GPIO 13 |
| Tactile button | Flash reset, GPIO 4 → GND |

Full pin-by-pin wiring: see **[WIRING.md](WIRING.md)**

---

## Setup

### 1. Wi-Fi credentials

```bash
cp TemperatureWatcher/secrets.h.example TemperatureWatcher/secrets.h
```

Edit `secrets.h`:

```cpp
#define WIFI_SSID     "your_network_name"
#define WIFI_PASSWORD "your_password"
```

`secrets.h` is gitignored and never committed.

### 2. Arduino IDE libraries

Install via **Library Manager** (Sketch → Include Library → Manage Libraries):

| Library | Version tested |
|---|---|
| Adafruit BMP085 Library | ≥ 1.2 |
| LiquidCrystal I2C | ≥ 1.1.2 |
| SPIMemory | ≥ 3.4 |

Board: **ESP32 Dev Module** (esp32 by Espressif, ≥ 2.x)

### 3. Flash and run

Select the correct COM port, upload. On first boot the device connects to Wi-Fi, syncs NTP (Kyiv timezone, UTC+2/+3), and starts the web server.

---

## Web Interface

Open the IP address shown on lcd2 in a browser.

| Route | Description |
|---|---|
| `/` | Live readings, auto-refreshes every 5 s |
| `/stats` | Last 50 records with temperature chart |
| `/export` | Download all records as `data.csv` |
| `/reset-flash` | Erase flash history (confirmation dialog) |
| `/api` | JSON endpoint: `{"temperature_c":…,"pressure_hpa":…,"pressure_mmhg":…,"altitude_m":…}` |

---

## Flash Data Storage

Records are written to W25Q64 every 2 minutes.

| Parameter | Value |
|---|---|
| Record size | 16 bytes (timestamp, temp, pressure hPa, altitude) |
| Max records | 524 032 |
| Total capacity | ~727 days at 2 min interval |
| Timestamp | Unix time, Kyiv timezone |

Records wrap around when full (oldest overwritten first). The `/stats` page shows the 50 most recent; `/export` downloads everything.

---

## LCD Layout

**LCD 2004 (20×4) — sensor data:**

```
Temp:   -12.3°C
hPa:   1013.2 hPa
mmHg:   759.9 mmH
Alt:    123.4 m
```

**LCD 1602 (16×2) — network info (normal):**

```
WiFi: MyNetwork
IP: 192.168.1.42
```

Long SSID or IP scrolls left↔right automatically.

**LCD 1602 during Wi-Fi loss / retry:**

```
WiFi:RETRY #1
Disconnected
```

Row 0 shows the retry counter, row 1 shows the status string from the Wi-Fi driver. Display updates on each new retry attempt (every 2 minutes). When the connection is restored lcd2 returns to the normal SSID/IP view and NTP is re-synced.

---

## Metadata Wear Leveling

The W25Q64 SPI flash stores sensor records starting at sector 1. Sector 0 is reserved for metadata (current write index and total record count). Flash memory has a limited erase endurance of approximately 100,000 cycles per sector.

### The Problem

A naive implementation erases sector 0 and rewrites it on every metadata update — which happens every 2 minutes alongside each sensor record save. At that rate:

- **100,000 erase cycles ÷ 720 writes/day ≈ 139 days** before sector 0 wears out, while the data capacity is designed for ~727 days.

### The Solution

Sector 0 (4,096 bytes) is divided into **512 slots of 8 bytes each**. Instead of erasing the sector on every write, the firmware appends the new metadata to the next free slot. The sector is only erased once all 512 slots are consumed, then writing restarts from slot 0.

This reduces the erase frequency by a factor of 512:

- **Every 2 min × 512 slots ≈ erase once every ~17 hours**
- **100,000 cycles × 17 h ≈ ~190 years** of sector lifetime

### How the Current Slot is Found at Boot

A slot is considered empty when both its words read as `0xFFFFFFFF` (the erased flash state). Because slots are always written sequentially, the sector layout is always:

```
[ valid | valid | ... | valid | empty | empty | ... | empty ]
```

This sorted structure allows a **binary search** to locate the first empty slot in **9 SPI reads** (log₂ 512) instead of up to 1,024 sequential reads. The slot immediately before the first empty one holds the current metadata.