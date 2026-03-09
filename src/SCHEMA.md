# SMS / CMS Serial Protocol Reference
**Version 2.0 — Station Management System**

---

## Table of Contents

1. [Overview](#1-overview)
2. [Transport & Framing](#2-transport--framing)
3. [State Machine](#3-state-machine)
4. [Occupancy Band Mapping](#4-occupancy-band-mapping)
5. [Commands — CMS → SMS](#5-commands--cms--sms)
   - 5.1 [Diagnostics](#51-diagnostics)
   - 5.2 [Configuration](#52-configuration)
   - 5.3 [LED Control](#53-led-control)
   - 5.4 [Train Lifecycle](#54-train-lifecycle)
6. [Responses & Events — SMS → CMS](#6-responses--events--sms--cms)
   - 6.1 [ACK](#61-ack)
   - 6.2 [Diagnostic Responses](#62-diagnostic-responses)
   - 6.3 [Weight Responses](#63-weight-responses)
   - 6.4 [Spontaneous Events](#64-spontaneous-events)
7. [Full Lifecycle Sequence](#7-full-lifecycle-sequence)
8. [Hardware Reference](#8-hardware-reference)
9. [Error Handling](#9-error-handling)

---

## 1. Overview

This document defines the full communication contract between:

- **CMS** — Central Management System (Python, web interface). Calculates occupancy, manages train schedules, sends commands.
- **SMS** — Station Management System (Arduino). Drives NeoPixel LED strip, reads HX711 load cells, reports weight data.

The CMS is always the initiator. The SMS responds to commands and also emits spontaneous events when weight changes or state transitions occur.

```
┌──────────────────────┐        Serial JSON        ┌──────────────────────┐
│         CMS          │ ────── commands ─────────► │         SMS          │
│  (Python / web UI)   │ ◄───── responses/events ── │  (Arduino / LEDs)    │
└──────────────────────┘                            └──────────────────────┘
```

---

## 2. Transport & Framing

| Property      | Value                        |
|---------------|------------------------------|
| Physical      | UART Serial                  |
| Baud rate     | 9600                         |
| Data bits     | 8                            |
| Stop bits     | 1                            |
| Parity        | None                         |
| Encoding      | UTF-8 (ASCII-safe subset)    |
| Framing       | One JSON object per line     |
| Line ending   | `\n` (LF)                    |
| Max line size | 256 bytes                    |

**Rules:**
- Every message is a single JSON object on one line, terminated by `\n`.
- The SMS reads characters until `\n` and then parses the full buffer.
- The CMS must not send a new command while an animation is in progress — the SMS serial buffer is only 64 bytes. Wait for the ACK before sending the next command.
- Commands that trigger animations (TRAIN_ARRIVING, TRAIN_LEAVING) ACK immediately, then emit a follow-up event when the animation completes.

---

## 3. State Machine

The SMS maintains one of five internal states. Many commands are only meaningful in specific states.

```
              ┌─────────────────────────────────────────┐
              │                                         │
     BOOT     ▼       TRAIN_EXPECTED                   │ TRAIN_EXPECTED_CLEAR
      │   ┌──────────┐ ──────────────► ┌────────────┐  │
      └──►│ NO_TRAIN │                 │  EXPECTED  │──┘
          │ (breath) │ ◄────────────── │(predicted  │
          └──────────┘  anim complete  │  colors)   │
                │                      └─────┬──────┘
                │                            │ TRAIN_ARRIVING
                │ TRAIN_ARRIVING             │
                │ (from no_train)            ▼
                └──────────────────► ┌────────────┐
                                     │  ARRIVING  │
                                     │ (animation)│
                                     └─────┬──────┘
                                           │ animation done
                                           ▼
                                     ┌────────────┐
                                     │ IN_SERVICE │◄── SET_ZONE / SET_ALL
                                     │ (live data)│◄── WEIGHT_CHANGE events
                                     └─────┬──────┘
                                           │ TRAIN_LEAVING
                                           ▼
                                     ┌────────────┐
                                     │  LEAVING   │
                                     │ (animation)│
                                     └─────┬──────┘
                                           │ animation done
                                           └──────────────► NO_TRAIN
```

| State        | LED behaviour                        | Weight events | Push active |
|--------------|--------------------------------------|---------------|-------------|
| `no_train`   | Slow blue breath pulse               | No            | No          |
| `expected`   | Static predicted occupancy colors    | No            | No          |
| `arriving`   | White pixel sweep animation          | No            | No          |
| `in_service` | Live occupancy colors                | Yes           | If enabled  |
| `leaving`    | Pixel-off sweep animation            | No            | No          |

---

## 4. Occupancy Band Mapping

The CMS is responsible for converting a weight percentage to a color and label. The SMS never decides occupancy — it only displays what the CMS sends.

| Band     | pct range | R   | G   | B   | Label    |
|----------|-----------|-----|-----|-----|----------|
| Free     | 0 – 49%   | 0   | 120 | 0   | `free`   |
| Medium   | 50 – 79%  | 120 | 60  | 0   | `medium` |
| Full     | 80 – 100% | 120 | 0   | 0   | `full`   |

These bands are the recommended defaults. The CMS may adjust thresholds per line, time of day, or operator preference.

---

## 5. Commands — CMS → SMS

All commands are JSON objects with a `"cmd"` field. Sent one per line, terminated by `\n`.

---

### 5.1 Diagnostics

---

#### PING

Check that the node is alive. No state change. CMS should send this periodically (e.g. every 30 seconds) and mark the node offline if no PONG is received within 2 seconds.

```json
{"cmd":"PING"}
```

**Response:** [PONG](#pong)

---

#### GET_STATUS

Request a full snapshot: train state, LED colors, labels, and live weights for all three zones. Use for dashboard refresh or initial sync after connect.

```json
{"cmd":"GET_STATUS"}
```

**Response:** [STATUS](#status)

---

#### GET_CONFIG

Request the current runtime configuration: calibration factors, max weights, push interval, brightness, and weight threshold.

```json
{"cmd":"GET_CONFIG"}
```

**Response:** [CONFIG](#config)

---

#### GET_WEIGHT

Request weight readings. Omit `zone` to get all three zones in one response.

```json
{"cmd":"GET_WEIGHT"}
{"cmd":"GET_WEIGHT","zone":2}
```

| Field  | Type    | Required | Description              |
|--------|---------|----------|--------------------------|
| `zone` | integer | No       | Zone 1–3. Omit for all.  |

**Response:** [WEIGHT (all)](#weight-all) or [WEIGHT (single)](#weight-single)

---

#### GET_RAW

Request raw ADC values from all HX711 chips with no calibration math applied. Use this during initial sensor calibration to find the correct `factor` for SET_CALIB.

**Procedure:** place a known weight (e.g. 10 kg) on the platform, call GET_RAW, then calculate `factor = raw / known_kg`.

```json
{"cmd":"GET_RAW"}
```

**Response:**
```json
{"type":"RAW","car":"A3","zones":[
  {"zone":1,"raw":104200},
  {"zone":2,"raw":103800},
  {"zone":3,"raw":104500}
]}
```

---

#### GET_UPTIME

Request time elapsed since last boot.

```json
{"cmd":"GET_UPTIME"}
```

**Response:**
```json
{"type":"UPTIME","car":"A3","uptime_ms":342100,"uptime_s":342}
```

---

#### SELFTEST

Flash each zone LED in sequence and report sensor connectivity. CMS must check `sensor_ok: true` on all zones before allowing train service. Safe to call in any state.

```json
{"cmd":"SELFTEST"}
```

**Response:** [SELFTEST](#selftest-response)

---

### 5.2 Configuration

All configuration changes are lost on reboot unless followed by [SAVE_CONFIG](#save_config).

---

#### SET_CAR_ID

Set the car identifier string. Included in all subsequent responses and events.

```json
{"cmd":"SET_CAR_ID","id":"A3"}
```

| Field | Type   | Required | Max length | Description        |
|-------|--------|----------|------------|--------------------|
| `id`  | string | Yes      | 7 chars    | Car identifier     |

**Response:** ACK

---

#### SET_CALIB

Set the calibration factor for one load cell. `factor` is raw ADC units per kilogram. Use GET_RAW with a known weight to calculate this value.

```json
{"cmd":"SET_CALIB","zone":1,"factor":418.5}
```

| Field    | Type    | Required | Description                    |
|----------|---------|----------|--------------------------------|
| `zone`   | integer | Yes      | Zone 1–3                       |
| `factor` | float   | Yes      | Raw ADC units per kg           |

**Response:** ACK

---

#### SET_MAX_KG

Set the reference weight for 100% occupancy on a zone. Affects the `pct` field in all weight responses.

```json
{"cmd":"SET_MAX_KG","zone":1,"kg":75.0}
```

| Field  | Type    | Required | Description                        |
|--------|---------|----------|------------------------------------|
| `zone` | integer | Yes      | Zone 1–3                           |
| `kg`   | float   | Yes      | Weight that equals 100% occupancy  |

**Response:** ACK

---

#### SET_THRESHOLD

Set the minimum weight change in kg that triggers a spontaneous WEIGHT_CHANGE event. Default: 2.0 kg.

```json
{"cmd":"SET_THRESHOLD","kg":2.0}
```

| Field | Type  | Required | Description              |
|-------|-------|----------|--------------------------|
| `kg`  | float | Yes      | Minimum delta to trigger |

**Response:** ACK

---

#### SET_BRIGHTNESS

Set global LED brightness. Useful for day/night adjustment. Does not affect the breath animation speed.

```json
{"cmd":"SET_BRIGHTNESS","value":180}
```

| Field   | Type    | Required | Range  | Description        |
|---------|---------|----------|--------|--------------------|
| `value` | integer | Yes      | 0–255  | Global brightness  |

**Response:** ACK

---

#### SET_PUSH_INTERVAL

Set the periodic weight push interval in milliseconds. The SMS will automatically send a WEIGHT (all zones) message at this interval while in `in_service` state. Set to 0 to disable.

```json
{"cmd":"SET_PUSH_INTERVAL","ms":3000}
```

| Field | Type    | Required | Range     | Description                  |
|-------|---------|----------|-----------|------------------------------|
| `ms`  | integer | Yes      | 0–60000   | Interval in ms. 0 = disabled |

**Response:** ACK

---

#### PUSH_ON

Shortcut to enable periodic push at 2000ms interval.

```json
{"cmd":"PUSH_ON"}
```

**Response:** ACK

---

#### PUSH_OFF

Shortcut to disable periodic push.

```json
{"cmd":"PUSH_OFF"}
```

**Response:** ACK

---

#### TARE

Zero out load cells. Call when the platform is empty. Omit `zone` to tare all three simultaneously.

```json
{"cmd":"TARE"}
{"cmd":"TARE","zone":2}
```

| Field  | Type    | Required | Description              |
|--------|---------|----------|--------------------------|
| `zone` | integer | No       | Zone 1–3. Omit for all.  |

**Response:** ACK

---

#### SAVE_CONFIG

Persist current configuration (car ID, calibration factors, max weights, threshold, push interval, brightness) to EEPROM. Call this after any SET_* command that must survive a reboot.

```json
{"cmd":"SAVE_CONFIG"}
```

**Response:** ACK

---

#### LOAD_CONFIG

Reload configuration from EEPROM, discarding any unsaved changes made since last boot or SAVE_CONFIG.

```json
{"cmd":"LOAD_CONFIG"}
```

**Response:** ACK

---

#### RESET

Soft-reboot the Arduino. The boot animation replays and a BOOT event is emitted. CMS should treat a BOOT event as a reconnect and re-run the startup sequence.

```json
{"cmd":"RESET"}
```

**Response:** ACK (sent before reboot)

---

### 5.3 LED Control

---

#### SET_ZONE

Set LED color and occupancy label for one zone.

- If `train_state` is `in_service` or `expected` — LEDs update immediately.
- If `train_state` is `no_train` — colors are staged silently and applied when the next TRAIN_ARRIVING animation completes.

```json
{"cmd":"SET_ZONE","zone":1,"r":0,"g":120,"b":0,"label":"free"}
```

| Field   | Type    | Required | Description                    |
|---------|---------|----------|--------------------------------|
| `zone`  | integer | Yes      | Zone 1–3                       |
| `r`     | integer | Yes      | Red 0–255                      |
| `g`     | integer | Yes      | Green 0–255                    |
| `b`     | integer | Yes      | Blue 0–255                     |
| `label` | string  | No       | `free`, `medium`, `full`, `unknown` |

**Response:** ACK

---

#### SET_ALL

Set LED colors and labels for all three zones in one command. Preferred over three sequential SET_ZONE calls to avoid flicker.

```json
{
  "cmd":"SET_ALL",
  "zones":[
    {"zone":1,"r":0,"g":120,"b":0,"label":"free"},
    {"zone":2,"r":120,"g":60,"b":0,"label":"medium"},
    {"zone":3,"r":120,"g":0,"b":0,"label":"full"}
  ]
}
```

| Field   | Type  | Required | Description                  |
|---------|-------|----------|------------------------------|
| `zones` | array | Yes      | 1–3 zone color objects       |

Each zone object follows the same fields as SET_ZONE.

**Response:** ACK

---

### 5.4 Train Lifecycle

---

#### TRAIN_EXPECTED

Notify the SMS that a train is expected. Stops the idle breath and displays predicted occupancy colors. Transitions state to `expected`.

The CMS calculates predicted occupancy (e.g. from the previous journey of the same train) and pushes the colors here. The SMS does not calculate predictions.

If the train is cancelled or delayed, follow with TRAIN_EXPECTED_CLEAR.

```json
{
  "cmd":"TRAIN_EXPECTED",
  "zones":[
    {"zone":1,"r":0,"g":120,"b":0,"label":"free"},
    {"zone":2,"r":120,"g":60,"b":0,"label":"medium"},
    {"zone":3,"r":120,"g":0,"b":0,"label":"full"}
  ]
}
```

**Response:** ACK

---

#### TRAIN_EXPECTED_CLEAR

Cancel the expected train. Returns SMS to `no_train` state and resumes idle breath. Only valid in `expected` state.

```json
{"cmd":"TRAIN_EXPECTED_CLEAR"}
```

**Response:** ACK

---

#### TRAIN_ARRIVING

Trigger arrival animation. Pixels light up one by one in direction of travel (white sweep), then settle to the staged zone colors. Valid from both `no_train` and `expected` states.

The ACK is sent immediately. The SMS emits a **TRAIN_IN_SERVICE** event when the animation finishes. CMS must wait for TRAIN_IN_SERVICE before sending TRAIN_BOARDING or SET_PUSH_INTERVAL.

```json
{"cmd":"TRAIN_ARRIVING","dir":"ltr"}
{"cmd":"TRAIN_ARRIVING","dir":"rtl"}
```

| Field | Type   | Required | Values        | Description               |
|-------|--------|----------|---------------|---------------------------|
| `dir` | string | No       | `ltr`, `rtl`  | Default: `ltr`            |

**Response:** ACK → then [TRAIN_IN_SERVICE event](#train_in_service)

---

#### TRAIN_BOARDING

Signal doors open. Plays a slow double-pulse animation on zone LEDs. Only meaningful in `in_service` state.

```json
{"cmd":"TRAIN_BOARDING"}
```

**Response:** ACK

---

#### TRAIN_DOORS_CLOSING

Warn passengers doors are closing. Plays 5× fast amber blink, then restores zone colors. Only meaningful in `in_service` state.

```json
{"cmd":"TRAIN_DOORS_CLOSING"}
```

**Response:** ACK

---

#### TRAIN_LEAVING

Trigger departure animation. Pixels turn off one by one in direction of travel. Valid from `in_service` state.

The ACK is sent immediately. The SMS emits a **TRAIN_GONE** event when the animation finishes. Idle breath resumes automatically.

```json
{"cmd":"TRAIN_LEAVING","dir":"ltr"}
{"cmd":"TRAIN_LEAVING","dir":"rtl"}
```

| Field | Type   | Required | Values        | Description               |
|-------|--------|----------|---------------|---------------------------|
| `dir` | string | No       | `ltr`, `rtl`  | Default: `ltr`            |

**Response:** ACK → then [TRAIN_GONE event](#train_gone)

---

## 6. Responses & Events — SMS → CMS

---

### 6.1 ACK

Every command receives exactly one ACK. On success `status` is `ok`. On failure `status` is `error` and `msg` describes the problem.

```json
{"type":"ACK","cmd":"SET_ZONE","status":"ok"}
{"type":"ACK","cmd":"SET_ZONE","status":"error","msg":"invalid zone"}
```

| Field    | Type   | Always present | Description                        |
|----------|--------|----------------|------------------------------------|
| `type`   | string | Yes            | Always `"ACK"`                     |
| `cmd`    | string | Yes            | The command being acknowledged     |
| `status` | string | Yes            | `"ok"` or `"error"`                |
| `msg`    | string | No             | Error detail. Only on `"error"`.   |

---

### 6.2 Diagnostic Responses

---

#### PONG

Response to PING.

```json
{"type":"PONG","car":"A3","uptime_ms":34210}
```

---

#### STATUS

Full node snapshot. Response to GET_STATUS.

```json
{
  "type":"STATUS",
  "car":"A3",
  "train_state":"in_service",
  "zones":[
    {"zone":1,"r":120,"g":0,"b":0,"label":"full","raw":198400,"kg":71.20,"pct":95},
    {"zone":2,"r":120,"g":60,"b":0,"label":"medium","raw":142000,"kg":48.30,"pct":64},
    {"zone":3,"r":0,"g":120,"b":0,"label":"free","raw":31200,"kg":12.10,"pct":16}
  ]
}
```

---

#### CONFIG

Current runtime configuration. Response to GET_CONFIG.

```json
{
  "type":"CONFIG",
  "car":"A3",
  "brightness":255,
  "push_interval_ms":3000,
  "weight_threshold":2.00,
  "zones":[
    {"zone":1,"calib":418.50,"max_kg":75.00},
    {"zone":2,"calib":421.00,"max_kg":75.00},
    {"zone":3,"calib":419.20,"max_kg":75.00}
  ]
}
```

---

#### SELFTEST response

Result of zone LED flash and sensor check. Response to SELFTEST. CMS must verify `sensor_ok: true` on all zones before proceeding to train service.

```json
{
  "type":"SELFTEST",
  "car":"A3",
  "zones":[
    {"zone":1,"sensor_ok":true,"raw":142,"kg":0.34},
    {"zone":2,"sensor_ok":true,"raw":98,"kg":0.23},
    {"zone":3,"sensor_ok":false,"raw":-1,"kg":0.00}
  ]
}
```

`raw` is -1 and `sensor_ok` is false if the HX711 chip did not respond.

---

### 6.3 Weight Responses

---

#### WEIGHT (all)

Response to `GET_WEIGHT` with no zone specified, or a periodic push message when push interval is active.

```json
{
  "type":"WEIGHT",
  "car":"A3",
  "zones":[
    {"zone":1,"raw":102400,"kg":42.50,"pct":57},
    {"zone":2,"raw":61200,"kg":25.40,"pct":34},
    {"zone":3,"raw":9800,"kg":4.00,"pct":5}
  ]
}
```

---

#### WEIGHT (single)

Response to `GET_WEIGHT` with a specific zone.

```json
{"type":"WEIGHT","car":"A3","zone":2,"raw":61200,"kg":25.40,"pct":34}
```

---

### 6.4 Spontaneous Events

These are emitted by the SMS without a preceding command.

---

#### BOOT

Emitted once on startup after boot animation and sensor init. CMS should treat this as a node reconnect and re-run the full startup sequence (PING → GET_CONFIG → TARE → SELFTEST).

```json
{"type":"EVENT","event":"BOOT","car":"A3","msg":"SMS ready"}
```

---

#### TRAIN_IN_SERVICE

Emitted when the TRAIN_ARRIVING animation completes and the node transitions to `in_service`. CMS must wait for this before sending TRAIN_BOARDING or SET_PUSH_INTERVAL.

```json
{"type":"EVENT","event":"TRAIN_IN_SERVICE","car":"A3"}
```

---

#### TRAIN_GONE

Emitted when the TRAIN_LEAVING animation completes and the node returns to `no_train`. Idle breath resumes automatically. CMS can now send TRAIN_EXPECTED for the next service.

```json
{"type":"EVENT","event":"TRAIN_GONE","car":"A3"}
```

---

#### WEIGHT_CHANGE

Emitted automatically when weight on a zone changes by more than `weight_threshold` kg. Only fired in `in_service` state. CMS should react by recalculating the occupancy band and sending SET_ZONE if the band has changed.

```json
{"type":"EVENT","event":"WEIGHT_CHANGE","car":"A3","zone":1,"kg":55.20,"pct":74}
```

---

## 7. Full Lifecycle Sequence

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 PHASE 1 — BOOT / HANDSHAKE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
CMS sends:   PING
SMS replies: PONG  (if no reply in 2s → node offline)

CMS sends:   GET_CONFIG
SMS replies: CONFIG  (CMS checks calibration values)

CMS sends:   SET_CALIB zone:1 factor:418.5
CMS sends:   SET_CALIB zone:2 factor:421.0
CMS sends:   SET_CALIB zone:3 factor:419.2
SMS replies: ACK × 3

CMS sends:   SET_MAX_KG zone:1 kg:75.0  (× 3)
CMS sends:   SET_CAR_ID id:"A3"
CMS sends:   TARE
CMS sends:   SAVE_CONFIG
SMS replies: ACK × 6

CMS sends:   SELFTEST
SMS replies: SELFTEST  (CMS asserts sensor_ok:true on all zones)

CMS sends:   GET_STATUS
SMS replies: STATUS  (dashboard initial sync)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 PHASE 2 — TRAIN EXPECTED  (2 min before arrival)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
CMS sends:   TRAIN_EXPECTED zones:[...]
SMS replies: ACK
             → LEDs show predicted occupancy colors
             → idle breath stops

  (if cancelled)
CMS sends:   TRAIN_EXPECTED_CLEAR
SMS replies: ACK
             → idle breath resumes

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 PHASE 3 — TRAIN ARRIVING
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
CMS sends:   TRAIN_ARRIVING dir:"ltr"
SMS replies: ACK
             → white pixel sweep animation plays
SMS emits:   EVENT TRAIN_IN_SERVICE
             → LEDs settle to zone colors

CMS sends:   TRAIN_BOARDING
SMS replies: ACK  → double-pulse animation

CMS sends:   SET_PUSH_INTERVAL ms:3000
SMS replies: ACK  → periodic WEIGHT messages begin

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 PHASE 4 — IN SERVICE  (passengers boarding)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
SMS emits:   EVENT WEIGHT_CHANGE zone:1 kg:55.2 pct:74
CMS sends:   SET_ZONE zone:1 r:120 g:60 b:0 label:"medium"

SMS emits:   EVENT WEIGHT_CHANGE zone:1 kg:71.0 pct:95
CMS sends:   SET_ZONE zone:1 r:120 g:0 b:0 label:"full"

SMS emits:   WEIGHT (all zones, every 3s)
CMS sends:   GET_STATUS  (dashboard poll, as needed)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 PHASE 5 — DEPARTURE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
CMS sends:   PUSH_OFF
SMS replies: ACK

CMS sends:   TRAIN_DOORS_CLOSING
SMS replies: ACK  → amber blink animation

CMS sends:   GET_WEIGHT
SMS replies: WEIGHT  (CMS logs final load for this service)

CMS sends:   TRAIN_LEAVING dir:"ltr"
SMS replies: ACK
             → pixel-off sweep animation plays
SMS emits:   EVENT TRAIN_GONE
             → LEDs dark, idle breath resumes
             → loop back to PHASE 2 for next service
```

---

## 8. Hardware Reference

### LED Strip Layout

```
Pixel:  0    1  2  3  4  5    6    7  8  9 10 11   12   13 14 15 16 17   18
        ▲    ←── Zone 1 ──→   ▲    ←──── Zone 2 ────→   ▲  ←── Zone 3 ──→  ▲
       sep                   sep                        sep               sep
```

Separator pixels (0, 6, 12, 18) are always black and never addressed by zone commands.

### Pin Assignments

| Signal        | Arduino Pin |
|---------------|-------------|
| NeoPixel data | 8           |
| HX711 #1 DOUT | 2           |
| HX711 #1 SCK  | 3           |
| HX711 #2 DOUT | 4           |
| HX711 #2 SCK  | 5           |
| HX711 #3 DOUT | 6           |
| HX711 #3 SCK  | 7           |

### EEPROM Layout

Config struct is written at address 0. Magic byte `0xAB` is checked on boot — if missing or mismatched, defaults are written. Bump the magic byte in firmware if the struct layout changes.

---

## 9. Error Handling

### Command errors

All errors return an ACK with `status: "error"` and a `msg` field.

| msg                    | Cause                                      |
|------------------------|--------------------------------------------|
| `missing cmd`          | JSON received but no `cmd` field           |
| `unknown command`      | `cmd` value not recognised                 |
| `invalid zone`         | `zone` field missing or not 1–3           |
| `missing r/g/b`        | SET_ZONE called without color fields       |
| `missing factor`       | SET_CALIB called without `factor`          |
| `missing kg`           | SET_MAX_KG or SET_THRESHOLD missing `kg`   |
| `missing value`        | SET_BRIGHTNESS missing `value`             |
| `missing ms`           | SET_PUSH_INTERVAL missing `ms`             |
| `missing id`           | SET_CAR_ID missing `id`                    |
| `no valid zones`       | SET_ALL zones array could not be parsed    |

### CMS timeout rules

| Situation                              | Action                                    |
|----------------------------------------|-------------------------------------------|
| No PONG within 2s of PING              | Mark node offline, retry after 5s         |
| No BOOT event within 5s of power-on   | Send PING to check                        |
| No TRAIN_IN_SERVICE within 10s        | Log warning, assume in_service anyway     |
| No TRAIN_GONE within 10s              | Log warning, assume no_train, send PING   |
| sensor_ok: false on SELFTEST           | Block train service, alert maintenance    |

### Buffer overflow prevention

The Arduino hardware serial buffer is 64 bytes. At 9600 baud this fills in ~53ms. To avoid overflow:

- Never send a new command until the ACK for the previous one is received.
- Do not send SET_ALL with all three zones during an active animation.
- Keep command JSON as compact as possible (no extra whitespace).