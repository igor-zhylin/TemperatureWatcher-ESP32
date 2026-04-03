# TemperatureWatcher-ESP32

ESP32-based weather station that reads temperature, atmospheric pressure, and altitude from a BMP180 sensor, logs every reading to an 8 MB SPI flash chip, and serves live data and history over Wi-Fi through a built-in web server.

---

## Project Structure

| File | Purpose |
|---|---|
| `TemperatureWatcher.ino` | Entry point — `setup()`, `loop()`, global state |
| `config.h` | Pin assignments and flash/timing constants |
| `flash.h` | W25Q64 flash structs, state, and all read/write functions |
| `lcd_display.h` | LCD objects, scroll state, and display helper functions |
| `web_handlers.h` | `WebServer` instance and all HTTP route handlers |
| `html_pages.h` | Static HTML for the live-data page and WiFi setup page (PROGMEM) |
| `html_stats.h` | Static head/CSS and footer for the history page |

---

## Features

- **BMP180 sensor** — temperature (°C), pressure (hPa / mmHg), altitude (m)
- **Two I2C LCD displays**
  - LCD 2004 (20×4) — live sensor readings, updated every 500 ms
  - LCD 1602 (16×2) — Wi-Fi SSID and IP address; scrolls long values
- **Web interface** — live readings, history with temperature chart, CSV export; consistent nav bar across all pages
- **8 MB SPI flash logging** — record saved every 2 minutes, capacity ~727 days
- **Flash wear leveling** — metadata sector endurance extended ~190 years (see below)
- **Physical reset button** — hold 2 s on GPIO 4 to erase all flash records
- **Wi-Fi provisioning** — on first boot (or after 15 failed attempts) the device starts an open AP `TempWatcher` at `192.168.0.1`; connect to it, open the browser, pick a network and enter the password; credentials are saved to flash and used on every subsequent boot
- **Wi-Fi reconnect** — if connection drops at runtime, retries every 2 minutes automatically; lcd2 shows retry counter and error code

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

Wi-Fi is configured at runtime via the built-in captive portal — no `secrets.h` needed.

On first boot (or whenever saved credentials are missing or fail 15 times):

1. The device starts an open Wi-Fi access point named **TempWatcher**
2. Connect to it from your phone or laptop
3. Open **`192.168.0.1`** in a browser (most phones open it automatically)
4. Select your network, enter the password, press **Save & Connect**
5. Credentials are written to flash — the device restarts and connects automatically from then on

To reconfigure, visit **`/api/wifi-setup`** from the main web interface at any time.

### 2. Arduino IDE libraries

Install via **Library Manager** (Sketch → Include Library → Manage Libraries):

| Library | Version tested |
|---|---|
| Adafruit BMP085 Library | ≥ 1.2 |
| LiquidCrystal I2C | ≥ 1.1.2 |
| SPIMemory | ≥ 3.4 |

Board: **ESP32 Dev Module** (esp32 by Espressif, ≥ 2.x)

### 3. Flash and run

Select the correct COM port, upload. On first boot the device starts in AP provisioning mode (see step 1). After credentials are saved it connects to Wi-Fi, syncs NTP (Kyiv timezone, UTC+2/+3), and starts the web server.

---

## Web Interface

Open the IP address shown on lcd2 in a browser. All pages share a navigation bar (Live / History / WiFi).

| Route | Description |
|---|---|
| `/` | Live readings, auto-refreshes every 5 s |
| `/api/stats` | Last 50 records with temperature chart and time axis |
| `/api/export` | Download all records as `data.csv` |
| `/api/reset-flash` | Erase flash history (confirmation dialog) |
| `/api/data` | JSON endpoint: `{"temperature_c":…,"pressure_hpa":…,"pressure_mmhg":…,"altitude_m":…}` |
| `/api/wifi-setup` | Wi-Fi provisioning page — scan networks, pick one, save credentials |

---

## Flash Data Storage

Records are written to W25Q64 every 2 minutes.

| Parameter | Value |
|---|---|
| Record size | 16 bytes (timestamp, temp, pressure hPa, altitude) |
| Max records | 523 776 |
| Total capacity | ~727 days at 2 min interval |
| Timestamp | Unix time, Kyiv timezone |

Records wrap around when full (oldest overwritten first). The `/api/stats` page shows the 50 most recent; `/api/export` downloads everything.

**Flash sector layout:**

| Sector | Address | Size | Purpose |
|---|---|---|---|
| 0 | 0 – 4 095 | 4 KB | Metadata wear-leveling (write index + record count) |
| 1 – 2046 | 4 096 – 8 384 511 | ~8 MB | Data records |
| 2047 | 8 384 512 – 8 388 607 | 4 KB | Wi-Fi credentials (SSID + password) |

Capacity calculation: `(8 388 608 − 4 096 − 4 096) ÷ 16 bytes = 523 776 records × 2 min = 727 days`

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

The W25Q64 SPI flash stores sensor records in sectors 1–2046. Sector 0 is reserved for metadata (current write index and total record count); sector 2047 is reserved for Wi-Fi credentials. Flash memory has a limited erase endurance of approximately 100,000 cycles per sector.

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