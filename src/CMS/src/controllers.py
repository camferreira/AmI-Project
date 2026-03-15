"""
Controller layer — business logic.

Responsibilities
----------------
* Owns the in-memory node_state (digital twin)
* Runs the background serial reader thread
* Applies incoming SMS messages → mutates state + persists via db
* Exposes send_command() and connect/disconnect helpers
* Emits SocketIO events (state, log) through the injected socketio handle
* Never imports Flask routing primitives
"""

import json
import threading
import time

import serial
import serial.tools.list_ports

import db

# ── In-memory node state ───────────────────────────────────────

_DEFAULT_ZONES = lambda: [
    {"zone": 1, "r": 0, "g": 120, "b": 0, "label": "free", "kg": 0.0, "pct": 0},
    {"zone": 2, "r": 0, "g": 120, "b": 0, "label": "free", "kg": 0.0, "pct": 0},
    {"zone": 3, "r": 0, "g": 120, "b": 0, "label": "free", "kg": 0.0, "pct": 0},
]

node_state: dict = {
    "car": "—",
    "train_state": "no_train",
    "serial_status": "disconnected",
    "zones": _DEFAULT_ZONES(),
}

# ── Serial state ───────────────────────────────────────────────

_ser: serial.Serial | None = None
_serial_lock = threading.Lock()
_serial_connected = False
_serial_port: str | None = None

# ── SocketIO back-reference (injected by app factory) ─────────

_sio = None   # set by init_controller()


def init_controller(socketio_instance) -> None:
    """Wire the SocketIO handle and start background threads.  Call once."""
    global _sio
    _sio = socketio_instance
    db.init_db()
    t = threading.Thread(target=_serial_reader, daemon=True)
    t.start()


# ── Helpers ────────────────────────────────────────────────────

def _emit_state() -> None:
    if _sio:
        _sio.emit("state", node_state)


def _log_event(direction: str, raw: str) -> None:
    car_id = node_state.get("car", "")
    entry = db.insert_event(direction, raw, car_id)
    if _sio:
        _sio.emit("log", entry)


# ── Incoming message handler ───────────────────────────────────

def apply_incoming(msg: dict) -> None:
    """Mutate node_state from a parsed SMS message, then persist + broadcast."""
    t = msg.get("type", "")

    if t == "STATUS":
        node_state["car"] = msg.get("car", node_state["car"])
        node_state["train_state"] = msg.get("train_state", node_state["train_state"])
        for z in msg.get("zones", []):
            idx = z["zone"] - 1
            node_state["zones"][idx].update(z)

    elif t == "WEIGHT":
        if "zones" in msg:
            for z in msg["zones"]:
                idx = z["zone"] - 1
                node_state["zones"][idx]["kg"] = z.get("kg", 0)
                node_state["zones"][idx]["pct"] = z.get("pct", 0)
        else:
            idx = msg["zone"] - 1
            node_state["zones"][idx]["kg"] = msg.get("kg", 0)
            node_state["zones"][idx]["pct"] = msg.get("pct", 0)

    elif t == "EVENT":
        ev = msg.get("event", "")
        node_state["car"] = msg.get("car", node_state["car"])

        if ev == "TRAIN_IN_SERVICE":
            node_state["train_state"] = "in_service"

        elif ev == "TRAIN_GONE":
            node_state["train_state"] = "no_train"
            for z in node_state["zones"]:
                z["r"], z["g"], z["b"] = 0, 0, 0
                z["label"] = "—"

        elif ev == "WEIGHT_CHANGE":
            idx = msg["zone"] - 1
            node_state["zones"][idx]["kg"] = msg.get("kg", 0)
            node_state["zones"][idx]["pct"] = msg.get("pct", 0)
            pct = msg.get("pct", 0)
            if pct >= 80:
                node_state["zones"][idx].update({"r": 120, "g": 0,   "b": 0,   "label": "full"})
            elif pct >= 50:
                node_state["zones"][idx].update({"r": 120, "g": 60,  "b": 0,   "label": "medium"})
            else:
                node_state["zones"][idx].update({"r": 0,   "g": 120, "b": 0,   "label": "free"})

    elif t == "PONG":
        node_state["car"] = msg.get("car", node_state["car"])

    # Persist snapshot and broadcast
    car_id = node_state.get("car", "")
    if car_id and car_id != "—":
        db.upsert_state(car_id, node_state)

    _emit_state()


# ── Serial background thread ───────────────────────────────────

def _serial_reader() -> None:
    global _ser, _serial_connected
    while True:
        if _ser and _ser.is_open:
            try:
                line = _ser.readline().decode("utf-8", errors="replace").strip()
                if line:
                    _log_event("rx", line)
                    try:
                        msg = json.loads(line)
                        apply_incoming(msg)
                    except json.JSONDecodeError:
                        pass
            except (serial.SerialException, OSError):
                _serial_connected = False
                node_state["serial_status"] = "disconnected"
                _emit_state()
                _ser = None
        else:
            time.sleep(0.5)


# ── Serial API (called by routes) ──────────────────────────────

def send_command(cmd: dict) -> bool:
    """Serialise *cmd* as JSON and write to the open serial port."""
    global _ser
    with _serial_lock:
        if _ser and _ser.is_open:
            try:
                raw = json.dumps(cmd, separators=(",", ":")) + "\n"
                _ser.write(raw.encode("utf-8"))
                _log_event("tx", raw)
                return True
            except (serial.SerialException, OSError):
                return False
    return False


def connect(port: str, baud: int = 9600) -> None:
    """Open serial port, update node_state, request initial sync."""
    global _ser, _serial_connected, _serial_port
    with _serial_lock:
        if _ser and _ser.is_open:
            _ser.close()
        _ser = serial.Serial(port, baud, timeout=1)
        _serial_connected = True
        _serial_port = port
        node_state["serial_status"] = "connected"

    _emit_state()
    time.sleep(1.5)       # let Arduino settle after DTR reset
    send_command({"cmd": "PING"})
    time.sleep(0.3)
    send_command({"cmd": "GET_STATUS"})


def disconnect() -> None:
    """Close serial port and update node_state."""
    global _ser, _serial_connected
    with _serial_lock:
        if _ser and _ser.is_open:
            _ser.close()
        _ser = None
        _serial_connected = False
        node_state["serial_status"] = "disconnected"
    _emit_state()


def list_ports() -> list[str]:
    return [p.device for p in serial.tools.list_ports.comports()]


# ── Log access ─────────────────────────────────────────────────

def get_log(limit: int = 50) -> list[dict]:
    car_id = node_state.get("car", "")
    return db.get_events(car_id, limit)
