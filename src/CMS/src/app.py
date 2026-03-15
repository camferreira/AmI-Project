"""
CMS — Central Management System
Application factory / entry point.

Architecture
------------
  app.py         ← entry point + app factory
  db.py          ← Model      : SQLite3 schema & CRUD
  controllers.py ← Controller : serial I/O, state mutations
  routes.py      ← View/Routes: Flask Blueprint + SocketIO handlers
  templates/
    index.html   ← UI
"""
import json
import time
import serial
import serial.tools.list_ports
import threading
from flask import Flask, render_template, request, jsonify
from flask_socketio import SocketIO, emit

# ── App setup ────────────────────────────────────────────────
app = Flask(__name__)
app.config["SECRET_KEY"] = "sms-cms-secret"
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

# ── Serial state ─────────────────────────────────────────────
ser = None
serial_lock = threading.Lock()
serial_connected = False
serial_port = None

# ── SMS node state (digital twin) ────────────────────────────
node_state = {
    "car": "—",
    "train_state": "no_train",
    "serial_status": "disconnected",
    "zones": [
        {"zone": 1, "r": 0, "g": 120, "b": 0, "label": "free", "kg": 0.0, "pct": 0},
        {"zone": 2, "r": 0, "g": 120, "b": 0, "label": "free", "kg": 0.0, "pct": 0},
        {"zone": 3, "r": 0, "g": 120, "b": 0, "label": "free", "kg": 0.0, "pct": 0},
    ],
}

event_log = []
MAX_LOG = 200

def log_event(direction: str, raw: str):
    entry = {
        "time": time.strftime("%H:%M:%S"),
        "direction": direction,
        "raw": raw.strip(),
    }
    event_log.append(entry)
    if len(event_log) > MAX_LOG:
        event_log.pop(0)
    socketio.emit("log", entry)


def apply_incoming(msg: dict):
    """Update node_state from an incoming SMS message."""
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
                node_state["zones"][idx]["kg"]  = z.get("kg", 0)
                node_state["zones"][idx]["pct"] = z.get("pct", 0)
        else:
            idx = msg["zone"] - 1
            node_state["zones"][idx]["kg"]  = msg.get("kg", 0)
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
            node_state["zones"][idx]["kg"]  = msg.get("kg", 0)
            node_state["zones"][idx]["pct"] = msg.get("pct", 0)
            # Map pct to color (mirrors SMS band logic)
            pct = msg.get("pct", 0)
            if pct >= 80:
                node_state["zones"][idx].update({"r": 120, "g": 0,   "b": 0,   "label": "full"})
            elif pct >= 50:
                node_state["zones"][idx].update({"r": 120, "g": 60,  "b": 0,   "label": "medium"})
            else:
                node_state["zones"][idx].update({"r": 0,   "g": 120, "b": 0,   "label": "free"})

    elif t == "PONG":
        node_state["car"] = msg.get("car", node_state["car"])

    # Push updated state to all clients
    socketio.emit("state", node_state)


# ── Serial thread ─────────────────────────────────────────────

def serial_reader():
    global ser, serial_connected
    while True:
        if ser and ser.is_open:
            try:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if line:
                    log_event("rx", line)
                    try:
                        msg = json.loads(line)
                        apply_incoming(msg)
                    except json.JSONDecodeError:
                        pass
            except (serial.SerialException, OSError):
                serial_connected = False
                node_state["serial_status"] = "disconnected"
                socketio.emit("state", node_state)
                ser = None
        else:
            time.sleep(0.5)


reader_thread = threading.Thread(target=serial_reader, daemon=True)
reader_thread.start()


# ── Serial API ────────────────────────────────────────────────

def send_command(cmd: dict) -> bool:
    global ser
    with serial_lock:
        if ser and ser.is_open:
            try:
                raw = json.dumps(cmd, separators=(",", ":")) + "\n"
                ser.write(raw.encode("utf-8"))
                log_event("tx", raw)
                return True
            except (serial.SerialException, OSError):
                return False
    return False


@app.route("/api/ports")
def list_ports():
    ports = [p.device for p in serial.tools.list_ports.comports()]
    return jsonify({"ports": ports})


@app.route("/api/connect", methods=["POST"])
def connect_serial():
    global ser, serial_connected, serial_port
    data = request.json
    port = data.get("port")
    baud = data.get("baud", 9600)
    try:
        with serial_lock:
            if ser and ser.is_open:
                ser.close()
            ser = serial.Serial(port, baud, timeout=1)
            serial_connected = True
            serial_port = port
            node_state["serial_status"] = "connected"
        socketio.emit("state", node_state)
        # Ask for immediate status sync
        time.sleep(1.5)  # let Arduino settle after DTR reset
        send_command({"cmd": "PING"})
        time.sleep(0.3)
        send_command({"cmd": "GET_STATUS"})
        return jsonify({"ok": True})
    except serial.SerialException as e:
        return jsonify({"ok": False, "error": str(e)}), 400


@app.route("/api/disconnect", methods=["POST"])
def disconnect_serial():
    global ser, serial_connected
    with serial_lock:
        if ser and ser.is_open:
            ser.close()
        ser = None
        serial_connected = False
        node_state["serial_status"] = "disconnected"
    socketio.emit("state", node_state)
    return jsonify({"ok": True})


@app.route("/api/command", methods=["POST"])
def command():
    cmd = request.json
    if not cmd:
        return jsonify({"ok": False, "error": "no payload"}), 400
    ok = send_command(cmd)
    return jsonify({"ok": ok})


@app.route("/api/state")
def get_state():
    return jsonify(node_state)


@app.route("/api/log")
def get_log():
    return jsonify(event_log)


# ── Pages ─────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


# ── SocketIO events ───────────────────────────────────────────

@socketio.on("connect")
def on_connect():
    emit("state", node_state)
    for entry in event_log[-50:]:
        emit("log", entry)


def main():
    socketio.run(app, host="0.0.0.0", port=5000, debug=False)


if __name__ == "__main__":
    main()
