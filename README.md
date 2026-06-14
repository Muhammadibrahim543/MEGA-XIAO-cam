# ESP32S3 CamUI — Instruction Manual
**Hardware:** Seeed XIAO ESP32-S3 Sense · ST7789 172×320 · MAX98357A · 3 Buttons

---

## 1. Wiring

### Buttons (active-LOW, GPIO pull-up)
| Button | GPIO | Function |
|--------|------|----------|
| UP     | 1    | Navigate up / increase value |
| OK     | 2    | Confirm (short) / Switch screen (hold 0.7s) |
| DN     | 3    | Navigate down / decrease value |

### ST7789 Display
Set pins in **TFT_eSPI → User_Setup.h**:
```cpp
#define ST7789_DRIVER
#define TFT_WIDTH   172
#define TFT_HEIGHT  320
#define TFT_MOSI    <pin>
#define TFT_SCLK    <pin>
#define TFT_CS      <pin>
#define TFT_DC      <pin>
#define TFT_RST     <pin>
#define SPI_FREQUENCY  40000000
```

### MAX98357A Speaker Amplifier
| MAX98357A | GPIO |
|-----------|------|
| BCLK      | 36   |
| LRC       | 35   |
| DIN       | 37   |
| GND       | GND  |
| VIN       | 3.3V or 5V |
| SD        | leave unconnected (left channel default) |

### SD Card
Onboard on XIAO Sense expansion board (CS=21, MOSI=9, MISO=8, SCK=7). No extra wiring needed.

### Microphone
Onboard XIAO Sense PDM mic (CLK=42, DATA=41). No extra wiring needed.

---

## 2. Libraries

Install via **Arduino Library Manager**:
- `TFT_eSPI` by Bodmer (≥ 2.5)

ESP32 Arduino core ≥ 3.0 provides everything else (`esp_camera`, `i2s_std`, `SD`, `EEPROM`).

**Board settings:**
- Board: `XIAO_ESP32S3`
- PSRAM: `OPI PSRAM`
- Flash Size: `8MB`
- Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)` or similar with ≥3MB app

---

## 3. Screen Layout

```
┌──────────────────┐ ← 172px
│                  │
│   LIVE / VIDEO   │ 172px  (letterboxed, any resolution)
│  [overlay HUD]   │
├──────────────────┤ ← 4px divider
│ MENU / HUD       │ 144px  (settings / files / playback info)
└──────────────────┘ 320px total
```

---

## 4. Screens & Navigation

### Hold OK (0.7s) to cycle screens:
`LIVE → SETTINGS → FILES → LIVE`

---

### LIVE (Viewfinder)
| Button | Action |
|--------|--------|
| OK (short) | **Start recording** |
| OK (short, while recording) | **Stop recording** |
| HOLD OK | → SETTINGS screen |

Recording saves `VID_0000.avi`, `VID_0001.avi` … to SD root.
- REC indicator blinks red (bottom-left of feed)
- Timer shows elapsed recording time

---

### SETTINGS
| Button | Navigation mode | Edit mode (amber highlight) |
|--------|-----------------|-----------------------------|
| UP     | Cursor up       | Increase / toggle value     |
| DN     | Cursor down     | Decrease / toggle value     |
| OK     | Enter edit mode | Confirm & exit edit mode    |
| HOLD OK | → FILES screen |                             |

Settings save to EEPROM automatically after each change.

#### All 17 Settings
| Setting | Range | Notes |
|---------|-------|-------|
| Resolution | 96×96 … UXGA | Camera reinits briefly |
| Quality | 4–63 | Lower = better (JPEG) |
| Brightness | −2 … +2 | |
| Contrast | −2 … +2 | |
| Saturation | −2 … +2 | |
| H-Mirror | ON/OFF | Flip horizontal |
| V-Flip | ON/OFF | Flip vertical |
| Auto WB | ON/OFF | Auto white balance |
| WB Mode | Auto/Sunny/Cloudy/Office/Home | |
| Auto Exp | ON/OFF | Auto exposure |
| Exp Value | 0–1200 | Manual exposure |
| Auto Gain | ON/OFF | Auto gain |
| Gain | 0–30 | Manual gain |
| Lens Corr | ON/OFF | Lens correction |
| Raw Gamma | ON/OFF | |
| BPC | ON/OFF | Black pixel cancel |
| WPC | ON/OFF | White pixel cancel |

---

### FILES
| Button | Action |
|--------|--------|
| UP/DN  | Select file |
| OK     | Play selected AVI |
| HOLD OK | → LIVE screen |

Lists all `.avi` files in SD root.

---

### PLAYBACK
| Button | Action |
|--------|--------|
| OK     | Pause / Resume |
| DN     | Stop & return to LIVE |
| HOLD OK | Stop & return to LIVE |

- Video is decoded frame-by-frame (MJPEG → RGB565)
- Audio plays through MAX98357A
- Progress bar shows playback position
- Auto-returns to LIVE when file ends

---

## 5. AVI File Format

Files recorded are **AVI 1.0** with:
- Video: **MJPEG** (standard, plays in VLC / Windows Media Player)
- Audio: **PCM 16-bit mono 16000 Hz**
- Compatible with most video players

---

## 6. Tips

- **Best recording FPS:** QVGA (320×240), Quality 10–15
- **Best quality recording:** SVGA+, Quality 8–12, Auto WB + Auto Exp ON
- **No SD card / mic:** system boots normally; recording silently disabled
- **Settings persist** across power cycles via EEPROM
- If display shows garbage: double-check TFT_eSPI User_Setup.h pin assignments
- PSRAM **must** be enabled in board settings (`OPI PSRAM` for Xiao S3)
