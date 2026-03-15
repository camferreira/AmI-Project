"""
Model layer — SQLite3 persistence.

Tables
------
event_log  — every serial RX/TX line (rolling window kept by MAX_LOG)
state_snap — latest node state snapshots, one row per car_id
"""

import json
import sqlite3
import threading
from datetime import datetime
from pathlib import Path

DB_PATH = Path(__file__).parent.parent / "cms.db"
MAX_LOG = 200           # maximum rows kept in event_log per car

_local = threading.local()   # thread-local connection cache


# ── Connection helpers ────────────────────────────────────────

def _get_conn() -> sqlite3.Connection:
    """Return a thread-local SQLite connection (created on first use)."""
    if not hasattr(_local, "conn") or _local.conn is None:
        conn = sqlite3.connect(DB_PATH, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA foreign_keys=ON")
        _local.conn = conn
    return _local.conn


def init_db() -> None:
    """Create tables if they don't exist yet.  Called once at startup."""
    conn = _get_conn()
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS event_log (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          TEXT    NOT NULL,
            direction   TEXT    NOT NULL CHECK(direction IN ('rx','tx')),
            raw         TEXT    NOT NULL,
            car_id      TEXT    NOT NULL DEFAULT ''
        );

        CREATE INDEX IF NOT EXISTS idx_event_log_ts
            ON event_log (ts DESC);

        CREATE TABLE IF NOT EXISTS state_snap (
            car_id      TEXT PRIMARY KEY,
            updated_at  TEXT NOT NULL,
            snapshot    TEXT NOT NULL   -- JSON blob of the full node_state dict
        );
    """)
    conn.commit()


# ── Event log ─────────────────────────────────────────────────

def insert_event(direction: str, raw: str, car_id: str = "") -> dict:
    """Persist one log entry and prune overflow rows.  Returns the new row."""
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    conn = _get_conn()
    conn.execute(
        "INSERT INTO event_log (ts, direction, raw, car_id) VALUES (?,?,?,?)",
        (ts, direction, raw.strip(), car_id),
    )
    # Keep at most MAX_LOG rows per car (delete oldest excess)
    conn.execute(
        """DELETE FROM event_log
           WHERE car_id = ?
             AND id NOT IN (
                 SELECT id FROM event_log
                 WHERE car_id = ?
                 ORDER BY id DESC
                 LIMIT ?
             )""",
        (car_id, car_id, MAX_LOG),
    )
    conn.commit()
    return {"time": ts, "direction": direction, "raw": raw.strip()}


def get_events(car_id: str = "", limit: int = 50) -> list[dict]:
    """Return the *limit* most recent log entries (newest last)."""
    conn = _get_conn()
    rows = conn.execute(
        """SELECT ts AS time, direction, raw
           FROM event_log
           WHERE car_id = ?
           ORDER BY id DESC
           LIMIT ?""",
        (car_id, limit),
    ).fetchall()
    # Reverse so oldest is first (like a terminal stream)
    return [dict(r) for r in reversed(rows)]


def clear_events(car_id: str = "") -> None:
    conn = _get_conn()
    conn.execute("DELETE FROM event_log WHERE car_id = ?", (car_id,))
    conn.commit()


# ── State snapshots ────────────────────────────────────────────

def upsert_state(car_id: str, state: dict) -> None:
    """Persist the full node_state dict for a car (INSERT OR REPLACE)."""
    conn = _get_conn()
    conn.execute(
        """INSERT INTO state_snap (car_id, updated_at, snapshot)
           VALUES (?, ?, ?)
           ON CONFLICT(car_id) DO UPDATE SET
               updated_at = excluded.updated_at,
               snapshot   = excluded.snapshot""",
        (car_id, datetime.utcnow().isoformat(), json.dumps(state)),
    )
    conn.commit()


def get_state(car_id: str) -> dict | None:
    """Return the last persisted state for *car_id*, or None."""
    conn = _get_conn()
    row = conn.execute(
        "SELECT snapshot FROM state_snap WHERE car_id = ?", (car_id,)
    ).fetchone()
    return json.loads(row["snapshot"]) if row else None


def list_cars() -> list[str]:
    """Return all car_ids that have a persisted state snapshot."""
    conn = _get_conn()
    rows = conn.execute("SELECT car_id FROM state_snap ORDER BY car_id").fetchall()
    return [r["car_id"] for r in rows]
