# SMS — Station Management System (Firmware)

> **Platform:** Arduino Uno R3  
> **Language:** C++ (Arduino framework)  
> **Main source:** `SMS_station.ino`  
> **Path:** `src/SMS/src/`

---

## Overview

The SMS firmware runs on an Arduino Uno R3 embedded inside a train carriage. It is responsible for:

- Reading passenger weight from three HX711 load-cell amplifiers (one per seating zone).
- Driving 75 WS2812B RGB LEDs across three zones to display occupancy status.
- Running animations for train lifecycle events (arriving, doors closing, departing).
- Persisting configuration (car ID, occupancy thresholds, LED brightness) to EEPROM.
- Communicating with the CMS over USB serial using a line-delimited JSON protocol.

---

## Files in This Directory

| File | Description |
|---|---|
| `SMS_station.ino` | Main Arduino sketch — all firmware logic |
| `diagram.json` | Wokwi circuit diagram (component positions and wiring) |
| `libraries.txt` | List of required Arduino libraries with versions |
| `wokwi-project.txt` | Wokwi simulator project configuration |
| `README.md` | This file |

---

## Hardware Requirements

| Component | Qty | Notes |
|---|---|---|
| Arduino Uno R3 | 1 | Or compatible clone |
| WS2812B LED strip | 1 | 75 LEDs total |
| HX711 load-cell amplifier | 3 | One per zone |
| Load cell 1kg | 3 | Calibrate `LC_SCALE` per cell |
| 5 V / ≥ 3 A power supply | 1 | Dedicated supply for LED strip recommended |
| 200–500 Ω resistor | 1 | In series on the LED data line |
| 1000 µF capacitor | 1 | Across LED strip power rails |

### Pin Assignments

| Function | Pin |
|---|---|
| LED strip data | D2 |
| Load cell 0 DOUT (Zone 1) | D3 |
| Load cell 0 CLK (Zone 1) | D4 |
| Load cell 1 DOUT (Zone 2) | D5 |
| Load cell 1 CLK (Zone 2) | D6 |
| Load cell 2 DOUT (Zone 3) | D7 |
| Load cell 2 CLK (Zone 3) | D8 |

---

## LED Layout

```
 Index : 0 ──────────── 24 | 25 ──────────── 49 | 50 ──────────── 74
 Zone  :       Zone 1       |      Zone 2        |      Zone 3
 LEDs  :         25         |         25         |         25
```

> `ZONE_START` / `ZONE_END` in the sketch use 1-indexed boundaries (`1–23`, `26–48`, `51–74`). LED indices 0, 25, and 50 serve as inter-zone separators and remain off.

---

## Software Requirements

### Arduino IDE

Arduino IDE version 2.x or later. Download from [arduino.cc](https://www.arduino.cc/en/software).

### Required Libraries

Listed in `libraries.txt`. Install both via **Sketch → Include Library → Manage Libraries…**

| Library | Author | Purpose |
|---|---|---|
| **FastLED** | Daniel Garcia | WS2812B LED control |
| **HX711** | Bogdan Necula | Load-cell amplifier interface |

---

## Setup & Flashing

### 1 — Install Libraries

Open the Arduino IDE Library Manager and install the libraries listed in `libraries.txt`.

### 2 — Calibrate the Load Cells

Each load cell has a slightly different gain. The constant `LC_SCALE` (default `7050`) converts raw HX711 output to kilograms. To calibrate:

1. Upload the sketch with the default value.
2. Place a known weight on a zone.
3. Send `{"cmd":"GET_WEIGHT"}` from the CMS and compare the reported `kg` to the actual weight.
4. Adjust: `new_scale = old_scale × (reported_kg / actual_kg)`.
5. Update `LC_SCALE` in the sketch and re-upload.

For best accuracy, calibrate each cell individually and store separate scale values.

### 3 — Configure the Car ID

The default car ID is `CAR1`, set in `configDefaults()`. To change it before first deployment, edit that function and re-upload. The ID is then stored in EEPROM and survives subsequent resets.

### 4 — Select Board and Port

In the Arduino IDE:
- **Tools → Board → Arduino AVR Boards → Arduino Uno**
- **Tools → Port → (your COM or /dev/tty port)**

### 5 — Upload

Click **Upload** (`Ctrl+U`). The boot animation — three sequential blue zone pulses — confirms a successful start. The Arduino then emits:

```json
{"type":"EVENT","event":"BOOT","car":"CAR1"}
```

---

## Simulation with Wokwi

No physical hardware is needed to run and test the firmware. The project can be simulated in the browser using [Wokwi](https://wokwi.com):

1. Go to [wokwi.com](https://wokwi.com) and create a new project, or import using `wokwi-project.txt`.
2. The component layout and wiring are defined in `diagram.json` — Wokwi loads this automatically.
3. Start the simulation. Use the Wokwi serial monitor to send JSON commands and observe responses.

---

## Serial Protocol (9600 baud, 8N1)

All messages are UTF-8 JSON terminated with `\n`. The receive buffer is 192 bytes — keep commands within this limit.

### Commands Accepted

#### Diagnostics

```jsonc
{"cmd":"PING"}
// → {"type":"PONG","car":"CAR1","uptime_ms":12345}

{"cmd":"GET_STATUS"}
// → {"type":"STATUS","car":"CAR1","train_state":"no_train","zones":[...]}

{"cmd":"GET_WEIGHT"}
// → {"type":"WEIGHT","zones":[{"zone":1,"kg":0.45,"pct":30}, ...]}

{"cmd":"SELFTEST"}
// → {"type":"SELFTEST","car":"CAR1","zones":[{"zone":1,"sensor_ok":true,"kg":0.45,"pct":30}, ...]}
```

#### LED Control

```jsonc
{"cmd":"SET_ZONE","zone":1,"r":0,"g":120,"b":0,"label":"free"}
// → {"type":"ACK","cmd":"SET_ZONE","status":"ok"}

{"cmd":"SET_ALL","zones":[
  {"zone":1,"r":120,"g":0,"b":0,"label":"full"},
  {"zone":2,"r":120,"g":60,"b":0,"label":"medium"},
  {"zone":3,"r":0,"g":120,"b":0,"label":"free"}
]}
// → {"type":"ACK","cmd":"SET_ALL","status":"ok"}
```

#### Train Lifecycle

```jsonc
{"cmd":"TRAIN_EXPECTED","zones":[...]}
// Pre-loads zone colours; displays them immediately.

{"cmd":"TRAIN_ARRIVING","dir":"ltr"}
// dir: "ltr" (left-to-right) or "rtl". Runs white sweep animation.
// → ACK, then EVENT: TRAIN_IN_SERVICE when animation completes.

{"cmd":"TRAIN_DOORS_CLOSING"}
// Amber strobe animation (5 flashes).

{"cmd":"TRAIN_LEAVING","dir":"ltr"}
// Runs black-sweep departure animation.
// → ACK, then EVENT: TRAIN_GONE when animation completes.
```

#### Configuration

```jsonc
{"cmd":"SET_BANDS","medium":40,"full":80}
// Sets occupancy thresholds in percent.

{"cmd":"SAVE_CONFIG"}
// Persists current config to EEPROM.

{"cmd":"LOAD_CONFIG"}
// Reloads config from EEPROM and re-applies LED brightness.

{"cmd":"RESET"}
// Software reset (jumps to address 0).
```

### Spontaneous Events (SMS → CMS)

The Arduino emits these without being asked:

```jsonc
{"type":"EVENT","event":"BOOT","car":"CAR1"}
{"type":"EVENT","event":"TRAIN_IN_SERVICE","car":"CAR1"}
{"type":"EVENT","event":"TRAIN_GONE","car":"CAR1"}
{"type":"EVENT","event":"WEIGHT_CHANGE","car":"CAR1","zone":2,"kg":1.20,"pct":80}
```

Weight-change events fire every 500 ms when a zone's reading shifts by more than 0.01 kg.

---

## EEPROM Layout

Config is stored from address `0` using the `Config` struct:

| Field | Type | Default | Description |
|---|---|---|---|
| `magic` | `uint8_t` | `0xAC` | Validity marker — triggers defaults if missing |
| `carId` | `char[6]` | `"CAR1"` | Node identifier |
| `maxKg[3]` | `float` | `0.010` | Per-zone max weight (kg) |
| `brightness` | `uint8_t` | `128` | LED brightness (0–255) |
| `pctMedium` | `uint8_t` | `40` | Medium occupancy threshold (%) |
| `pctFull` | `uint8_t` | `80` | Full occupancy threshold (%) |

If the magic byte is absent on boot, defaults are written automatically.

---

## Occupancy Colour Logic

The firmware maps weight percentage to zone colour automatically during `IN_SERVICE` and `EXPECTED` states:

| Percentage | Colour | Label |
|---|---|---|
| < `pctMedium` | 🟢 RGB `(0, 120, 0)` | `free` |
| `pctMedium` – `pctFull` | 🟠 RGB `(120, 60, 0)` | `medium` |
| ≥ `pctFull` | 🔴 RGB `(120, 0, 0)` | `full` |

LEDs are **not** updated while `train_state` is `NO_TRAIN` or `LEAVING`.

---

## Known Limitations

- `LC_SCALE` is a single constant shared by all three load cells. For greater accuracy, each cell should have its own calibration factor.
- The 192-byte receive buffer can be saturated by large `SET_ALL` payloads with long label strings.
- `RESET` uses `asm volatile("jmp 0")`, which restarts firmware execution but does not reinitialise hardware peripherals as cleanly as a power cycle.
- The weight-change loop uses `get_units(1)` (single sample) for speed; `GET_WEIGHT` uses `get_units(5)` for a more stable average.