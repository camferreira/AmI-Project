"""
View/Routes layer — Flask Blueprint + SocketIO handlers.

All handlers are thin:  validate input → call controller → return response.
No business logic lives here.
"""

import serial
from flask import Blueprint, jsonify, render_template, request
from flask_socketio import SocketIO, emit

import controllers
import db

bp = Blueprint("cms", __name__)

# SocketIO handle — injected by register_routes()
_sio: SocketIO | None = None


def register_routes(app, socketio: SocketIO) -> None:
    """Register the blueprint and SocketIO handlers on *app*."""
    global _sio
    _sio = socketio
    app.register_blueprint(bp)
    _register_socket_handlers(socketio)


# ── Pages ─────────────────────────────────────────────────────

@bp.route("/")
def index():
    return render_template("index.html")


# ── Serial API ─────────────────────────────────────────────────

@bp.route("/api/ports")
def api_ports():
    return jsonify({"ports": controllers.list_ports()})


@bp.route("/api/connect", methods=["POST"])
def api_connect():
    data = request.get_json(force=True) or {}
    port = data.get("port")
    baud = int(data.get("baud", 9600))
    if not port:
        return jsonify({"ok": False, "error": "port required"}), 400
    try:
        controllers.connect(port, baud)
        return jsonify({"ok": True})
    except serial.SerialException as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400


@bp.route("/api/disconnect", methods=["POST"])
def api_disconnect():
    controllers.disconnect()
    return jsonify({"ok": True})


@bp.route("/api/command", methods=["POST"])
def api_command():
    cmd = request.get_json(force=True)
    if not cmd:
        return jsonify({"ok": False, "error": "no payload"}), 400
    ok = controllers.send_command(cmd)
    return jsonify({"ok": ok})


# ── State & Log ────────────────────────────────────────────────

@bp.route("/api/state")
def api_state():
    return jsonify(controllers.node_state)


@bp.route("/api/log")
def api_log():
    limit = int(request.args.get("limit", 50))
    return jsonify(controllers.get_log(limit))


# ── DB helpers ─────────────────────────────────────────────────

@bp.route("/api/cars")
def api_cars():
    """Return all car IDs that have a persisted state snapshot."""
    return jsonify({"cars": db.list_cars()})


@bp.route("/api/cars/<car_id>/state")
def api_car_state(car_id: str):
    """Return the last persisted state snapshot for a specific car."""
    state = db.get_state(car_id)
    if state is None:
        return jsonify({"error": "not found"}), 404
    return jsonify(state)


@bp.route("/api/cars/<car_id>/log")
def api_car_log(car_id: str):
    limit = int(request.args.get("limit", 50))
    return jsonify(db.get_events(car_id, limit))


# ── SocketIO handlers ──────────────────────────────────────────

def _register_socket_handlers(socketio: SocketIO) -> None:

    @socketio.on("connect")
    def on_connect():
        emit("state", controllers.node_state)
        for entry in controllers.get_log(50):
            emit("log", entry)
