# TemperatureWatcher-ESP32

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