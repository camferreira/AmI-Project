# Station Management System (SMS/CMS) — Project Overview

> **Version:** 4.0 — Simulation Build  
> **Architecture:** Distributed embedded + web  
> **Protocol:** JSON over Serial (9600 baud, `\n` terminated)

---

## What This Project Does

This project is a two-component system for real-time passenger occupancy monitoring inside a train carriage:

- **SMS (Station Management System)** — firmware running on an Arduino Uno R3. It reads weight from three load-cell sensors (one per seating zone), drives a strip of 75 WS2812B RGB LEDs to signal zone occupancy, and communicates with the CMS over USB serial using a JSON protocol.
- **CMS (Central Management System)** — a Python/Flask web application that connects to the Arduino over serial, displays a live dashboard of zone states and sensor weights, and allows an operator to send commands (train lifecycle events, zone overrides, configuration changes).

---

## Repository Structure

```
/
├── README.md                        ← You are here
└── src/
    ├── SCHEMA.md                    ← JSON protocol schema reference
    ├── SMS/
    │   └── src/
    │       ├── README.md            ← SMS firmware documentation
    │       ├── SMS_station.ino      ← Arduino sketch (main source)
    │       ├── diagram.json         ← Wokwi circuit diagram
    │       ├── libraries.txt        ← Required Arduino libraries
    │       └── wokwi-project.txt    ← Wokwi simulator project config
    └── CMS/
        └── src/
            ├── README.md            ← CMS application documentation
            ├── app.py               ← Flask + SocketIO server
            ├── pyproject.toml       ← Python project metadata and dependencies
            ├── uv.lock              ← Locked dependency versions (uv)
            └── templates/
                └── index.html       ← Web dashboard
```

---

## System Architecture

```
┌───────────────────────────────────────────────────────────────┐
│                      Train Carriage                           │
│                                                               │
│  Zone 1 (LEDs 0–24)  Zone 2 (LEDs 25–49)  Zone 3 (LEDs 50–74) │
│  Load Cell (HX711)   Load Cell (HX711)    Load Cell (HX711)   │
│                                                               │
│                    Arduino Uno R3                             │
└──────────────────────────┬────────────────────────────────────┘
                           │ USB Serial
               JSON over Serial (9600 baud, \n)
                           │
              ┌────────────▼────────────┐
              │   CMS — Flask Server    │
              │   (Python, port 5050)   │
              └────────────┬────────────┘
                           │ WebSocket (Socket.IO)
              ┌────────────▼────────────┐
              │   Operator Dashboard    │
              │      (Web Browser)      │
              └─────────────────────────┘
```

---

## Hardware Requirements

| Component | Specification |
|---|---|
| Microcontroller | Arduino Uno R3 |
| LED strip | WS2812B, 75 LEDs |
| Load cells | × 3, any compatible cells |
| Load cell amplifiers | HX711 × 3 |
| Power supply | 5 V, 10 A (for LEDs) |
| USB cable | USB-A to USB-B (Arduino connection) |

### Wiring Summary

| Signal | Arduino Pin |
|---|---|
| LED data | D2 |
| Load cell 0 DOUT (Zone 1) | D3 |
| Load cell 0 CLK (Zone 1) | D4 |
| Load cell 1 DOUT (Zone 2) | D5 |
| Load cell 1 CLK (Zone 2) | D6 |
| Load cell 2 DOUT (Zone 3) | D7 |
| Load cell 2 CLK (Zone 3) | D8 |

The full circuit definition is in `src/SMS/src/diagram.json` (Wokwi format).

---

## Software Requirements

| Layer | Technology | Version |
|---|---|---|
| Firmware IDE | Arduino IDE or Wokwi simulator | IDE ≥ 2.x |
| Firmware libraries | FastLED, HX711 | See `src/SMS/src/libraries.txt` |
| Backend runtime | Python | ≥ 3.9 |
| Backend package manager | uv | ≥ 0.4 (or plain pip) |
| Frontend | Any modern browser | — |

---

## Quick Start

### 1 — Flash the Arduino

See `src/SMS/src/README.md` for full instructions. In brief:

```bash
# Open src/SMS/src/SMS_station.ino in the Arduino IDE
# Install libraries listed in src/SMS/src/libraries.txt via Library Manager
# Select board: Arduino Uno, correct COM/tty port
# Upload
```

Alternatively, open the project in the [Wokwi simulator](https://wokwi.com) using `wokwi-project.txt`.

### 2 — Start the CMS server

See `src/CMS/src/README.md` for full instructions. In brief:

```bash
cd src/CMS/src

# With uv (recommended — reads pyproject.toml automatically)
uv run app.py

# Or with plain pip
pip install flask flask-socketio pyserial
python app.py
```

Then open `http://localhost:5050` in your browser.

### 3 — Connect

In the web dashboard, select the serial port the Arduino is on and click **Connect**. The system automatically pings the node and requests its current status.

---

## JSON Communication Protocol

All messages are single-line JSON terminated with `\n`. See `src/SCHEMA.md` for the full schema.

### CMS → SMS (Commands)

| Command | Description |
|---|---|
| `PING` | Health check; SMS replies with `PONG` |
| `GET_STATUS` | Request full zone and train state |
| `GET_WEIGHT` | Request fresh sensor readings |
| `SET_ZONE` | Override a single zone colour and label |
| `SET_ALL` | Override all zones in one message |
| `SET_BANDS` | Update occupancy thresholds (%) |
| `TRAIN_EXPECTED` | Pre-load zone colours before train arrives |
| `TRAIN_ARRIVING` | Trigger arrival animation |
| `TRAIN_DOORS_CLOSING` | Trigger doors-closing animation |
| `TRAIN_LEAVING` | Trigger departure animation |
| `SAVE_CONFIG` | Persist configuration to EEPROM |
| `LOAD_CONFIG` | Reload configuration from EEPROM |
| `SELFTEST` | Cycle all LEDs white and report sensor readings |
| `RESET` | Software reset the Arduino |

### SMS → CMS (Responses / Events)

| Type | When emitted |
|---|---|
| `PONG` | Response to `PING` |
| `STATUS` | Response to `GET_STATUS` |
| `WEIGHT` | Response to `GET_WEIGHT` |
| `ACK` | Acknowledgement of any command |
| `SELFTEST` | Response to `SELFTEST` |
| `EVENT` | Spontaneous: `BOOT`, `TRAIN_IN_SERVICE`, `TRAIN_GONE`, `WEIGHT_CHANGE` |

---

## Zone Occupancy Colour Coding

| Colour | Label | Default threshold |
|---|---|---|
| 🟢 Green | `free` | < 40 % |
| 🟠 Amber | `medium` | 40 – 79 % |
| 🔴 Red | `full` | ≥ 80 % |

Thresholds are configurable via `SET_BANDS` and persisted to EEPROM with `SAVE_CONFIG`.

---

## Simulation (Wokwi)

The SMS component can be run entirely in the browser without physical hardware using the [Wokwi Arduino simulator](https://wokwi.com):

1. Open Wokwi and import the project using `src/SMS/src/wokwi-project.txt`.
2. The circuit layout is defined in `src/SMS/src/diagram.json`.
3. Start the simulation — the serial monitor can be used to send and receive JSON commands manually.

