#!/usr/bin/env python3
"""R2's persistent virtual Game Boy console.

This is the device layer, not an emulator. It delegates game execution to mGBA
and stores only the inserted cartridge plus console/session state in gameboy.db.
No emulator save-state or rewind commands are exposed by this interface.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import shutil
import sqlite3
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("R2_GAMEBOY_ROOT", os.environ.get("R2_GAMEBOY_DIR", str(Path(__file__).resolve().parent)))).resolve()
CARTRIDGES = ROOT / "Cartridges"
SAVES = ROOT / "Saves"
STATE = ROOT / "State"
DB_PATH = Path(os.environ.get("R2_GAMEBOY_DB", str(ROOT / "gameboy.db")))
STATE_DB_PATH = STATE / "console_state.db"
EMULATOR = os.environ.get("R2_MGBA_EXECUTABLE", "/usr/games/mgba-qt")
ROM_EXTENSIONS = {".gba", ".gb", ".gbc"}
VERIFIED_GAME_EVENT_TYPES = {
    "character_jumped", "coin_collected", "item_collected", "life_gained",
    "life_lost", "damage_taken", "level_started", "level_completed",
    "game_completed", "screen_transition",
}
BUTTON_KEYS = {
    "A": "x", "B": "z", "L": "a", "R": "s",
    "START": "Return", "SELECT": "BackSpace",
    "UP": "Up", "DOWN": "Down", "LEFT": "Left", "RIGHT": "Right",
}
DEFAULT_PRESS_MS = 120
MAX_PRESS_MS = 5000

# Keep newly created DB/WAL/save files group-writable for the shared r2 group.
os.umask(0o002)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def ensure_dirs() -> None:
    for path in (CARTRIDGES, SAVES, STATE, STATE / "mgba-config" / "mgba",
                 DB_PATH.parent, STATE_DB_PATH.parent):
        path.mkdir(parents=True, exist_ok=True)


def connect() -> sqlite3.Connection:
    ensure_dirs()
    db = sqlite3.connect(STATE_DB_PATH, timeout=5.0)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA busy_timeout=5000")
    db.execute("PRAGMA journal_mode=WAL")
    # gameboy.db is intentionally only the cartridge slot. Runtime/power state
    # and the event history live in State/console_state.db instead.
    db.execute("ATTACH DATABASE ? AS cartridge", (str(DB_PATH),))
    db.executescript("""
        CREATE TABLE IF NOT EXISTS cartridge.cartridge_slot (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            rom_path TEXT,
            rom_title TEXT,
            inserted_at TEXT
        );
        INSERT OR IGNORE INTO cartridge.cartridge_slot(id) VALUES (1);
        CREATE TABLE IF NOT EXISTS console_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            power_state TEXT NOT NULL DEFAULT 'off'
                CHECK (power_state IN ('off','on')),
            emulator_pid INTEGER,
            emulator_start_ticks INTEGER,
            game_started_at TEXT,
            session_id TEXT
        );
        INSERT OR IGNORE INTO console_state(id, power_state) VALUES (1, 'off');
        CREATE TABLE IF NOT EXISTS console_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            context TEXT NOT NULL CHECK (context IN ('physical','virtual','system')),
            event_type TEXT NOT NULL,
            summary TEXT NOT NULL,
            details TEXT,
            game_title TEXT,
            session_id TEXT,
            evidence_source TEXT,
            verified INTEGER NOT NULL DEFAULT 0,
            life_log_synced INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS console_events_time_idx
            ON console_events(id);
        CREATE TABLE IF NOT EXISTS cartridge_inventory (
            filename TEXT PRIMARY KEY COLLATE NOCASE,
            title TEXT NOT NULL,
            location TEXT NOT NULL DEFAULT 'pockets'
                CHECK (location IN ('pockets','shelf','box','room','slot')),
            discovered_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
    """)
    # Migrate a device database created by an earlier console build.
    columns = {row[1] for row in db.execute("PRAGMA table_info(console_events)")}
    for name, declaration in (
        ("evidence_source", "TEXT"),
        ("verified", "INTEGER NOT NULL DEFAULT 0"),
        ("life_log_synced", "INTEGER NOT NULL DEFAULT 0"),
    ):
        if name not in columns:
            db.execute(f"ALTER TABLE console_events ADD COLUMN {name} {declaration}")
    db.commit()
    return db


def record_event(db: sqlite3.Connection, context: str, event_type: str,
                 summary: str, details: str | None = None,
                 game_title: str | None = None, session_id: str | None = None,
                 evidence_source: str | None = None, verified: bool = False) -> None:
    db.execute(
        "INSERT INTO console_events(timestamp,context,event_type,summary,details,game_title,session_id,"
        "evidence_source,verified,life_log_synced) VALUES(?,?,?,?,?,?,?,?,?,0)",
        (utc_now(), context, event_type, summary, details, game_title, session_id,
         evidence_source, 1 if verified else 0),
    )
    db.commit()


def record_verified_game_event(db: sqlite3.Connection, event_type: str,
                               summary: str, details: str,
                               evidence_source: str) -> int:
    """Record an event detected by a trusted game-specific adapter.

    This API is intentionally not exposed as an R2 [WORLD] action. A model
    statement alone is not evidence that a character performed an action.
    """
    row = reconcile(db)
    if row["power_state"] != "on" or not row["emulator_pid"]:
        raise ValueError("No active game session; virtual events cannot be recorded.")
    if event_type not in VERIFIED_GAME_EVENT_TYPES:
        raise ValueError("Unsupported verified game event type.")
    if not summary or len(summary) > 1000 or len(details or "") > 3000:
        raise ValueError("Event summary/details are empty or too long.")
    if not evidence_source or not evidence_source.startswith("adapter:"):
        raise ValueError("A game adapter evidence source is required.")
    record_event(db, "virtual", event_type, summary, details,
                 row["cartridge_title"], row["session_id"],
                 evidence_source=evidence_source, verified=True)
    return int(db.execute("SELECT last_insert_rowid()").fetchone()[0])


def pending_verified_events(db: sqlite3.Connection) -> list[dict[str, Any]]:
    rows = db.execute(
        "SELECT id,timestamp,event_type,summary,details,game_title,session_id,evidence_source "
        "FROM console_events WHERE context='virtual' AND verified=1 AND life_log_synced=0 "
        "ORDER BY id"
    ).fetchall()
    return [{
        "event_id": row["id"], "timestamp": row["timestamp"],
        "event_type": row["event_type"], "summary": row["summary"],
        "details": row["details"], "game_title": row["game_title"],
        "session_id": row["session_id"], "evidence_source": row["evidence_source"],
        "context": "virtual", "verified": True,
    } for row in rows]


def acknowledge_verified_event(db: sqlite3.Connection, event_id: int) -> bool:
    cursor = db.execute(
        "UPDATE console_events SET life_log_synced=1 "
        "WHERE id=? AND context='virtual' AND verified=1 AND life_log_synced=0",
        (event_id,),
    )
    db.commit()
    return cursor.rowcount == 1


def process_start_ticks(pid: int) -> int | None:
    """Linux /proc start time, used to avoid mistaking a recycled PID for mGBA."""
    try:
        raw = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        tail = raw[raw.rfind(")") + 2:].split()
        # tail starts at proc stat field 3; starttime is field 22.
        return int(tail[19])
    except (OSError, ValueError, IndexError):
        return None


def process_matches(pid: int | None, expected_ticks: int | None) -> bool:
    if not pid or pid <= 1:
        return False
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError, OSError):
        return False
    actual = process_start_ticks(pid)
    return actual is not None and expected_ticks is not None and actual == expected_ticks


def state_row(db: sqlite3.Connection) -> sqlite3.Row:
    row = db.execute(
        "SELECT s.*, c.rom_path AS cartridge_path, c.rom_title AS cartridge_title, "
        "c.inserted_at AS cartridge_inserted_at "
        "FROM console_state AS s CROSS JOIN cartridge.cartridge_slot AS c "
        "WHERE s.id=1 AND c.id=1"
    ).fetchone()
    assert row is not None
    return row


def reconcile(db: sqlite3.Connection) -> sqlite3.Row:
    row = state_row(db)
    pid = row["emulator_pid"]
    if row["power_state"] == "on" and pid is not None and not process_matches(
        int(pid), row["emulator_start_ticks"]
    ):
        title = row["cartridge_title"] or "unknown game"
        session_id = row["session_id"]
        db.execute(
            "UPDATE console_state SET power_state='off', emulator_pid=NULL, "
            "emulator_start_ticks=NULL, game_started_at=NULL, session_id=NULL WHERE id=1"
        )
        record_event(
            db, "system", "emulator_exited",
            f"The emulator for {title} is no longer running.",
            "Detected by checking the stored process ID and Linux process start time.",
            title, session_id,
        )
        db.commit()
    return state_row(db)


def read_rom_title(path: Path) -> str:
    try:
        data = path.read_bytes()[:0x200]
        if path.suffix.lower() == ".gba" and len(data) >= 0xAC:
            raw = data[0xA0:0xAC]
        elif path.suffix.lower() in {".gb", ".gbc"} and len(data) >= 0x144:
            raw = data[0x134:0x144]
        else:
            raw = b""
        title = raw.split(b"\0", 1)[0].decode("ascii", "ignore").strip()
        title = "".join(ch for ch in title if ch.isprintable()).strip()
        return title or path.stem
    except OSError:
        return path.stem


def safe_cartridge(name: str) -> Path:
    # Cartridge insertion accepts a loose filename, never an arbitrary path.
    if not name or Path(name).name != name or name in {".", ".."}:
        raise ValueError("Specify a cartridge filename from the Cartridges folder, not a path.")
    path = (CARTRIDGES / name).resolve()
    if path.parent != CARTRIDGES.resolve():
        raise ValueError("Cartridge must be inside the console's Cartridges folder.")
    if path.suffix.lower() not in ROM_EXTENSIONS:
        raise ValueError("Supported cartridge formats are .gba, .gb, and .gbc.")
    if not path.is_file():
        raise ValueError(f"No cartridge file named '{name}' exists in Cartridges.")
    if path.stat().st_size < 0x150:
        raise ValueError("The cartridge file is too small to be a valid GB/GBC/GBA ROM.")
    return path


def sync_cartridge_inventory(db: sqlite3.Connection) -> None:
    """Discover loose cartridge files without moving previously stored games."""
    ensure_dirs()
    now = utc_now()
    for path in sorted(CARTRIDGES.iterdir(), key=lambda p: p.name.casefold()):
        if not path.is_file() or path.suffix.lower() not in ROM_EXTENSIONS:
            continue
        title = read_rom_title(path)
        db.execute(
            "INSERT INTO cartridge_inventory(filename,title,location,discovered_at,updated_at) "
            "VALUES(?,?,'pockets',?,?) ON CONFLICT(filename) DO UPDATE SET "
            "title=excluded.title,updated_at=excluded.updated_at",
            (path.name, title, now, now),
        )
    slot = db.execute("SELECT rom_path,rom_title FROM cartridge.cartridge_slot WHERE id=1").fetchone()
    if slot and slot["rom_path"]:
        filename = Path(slot["rom_path"]).name
        title = slot["rom_title"] or Path(filename).stem
        db.execute(
            "INSERT INTO cartridge_inventory(filename,title,location,discovered_at,updated_at) "
            "VALUES(?,?,'slot',?,?) ON CONFLICT(filename) DO UPDATE SET "
            "title=excluded.title,location='slot',updated_at=excluded.updated_at",
            (filename, title, now, now),
        )
    db.commit()


def move_cartridge(db: sqlite3.Connection, filename: str, location: str) -> dict[str, Any]:
    """Move one loose cartridge between R2's physical inventory locations."""
    row = reconcile(db)
    if row["power_state"] != "off":
        return {"ok": False, "message": "Power off the console before moving cartridges.", **status_data(db)}
    destination = location.strip().lower()
    if destination not in {"pockets", "shelf", "box", "room"}:
        return {"ok": False, "message": "Destination must be pockets, shelf, box, or room.", **status_data(db)}
    if not filename or Path(filename).name != filename or filename in {".", ".."}:
        return {"ok": False, "message": "Use a cartridge filename, not a path.", **status_data(db)}
    sync_cartridge_inventory(db)
    current = db.execute("SELECT location,title FROM cartridge_inventory WHERE filename=? COLLATE NOCASE", (filename,)).fetchone()
    if current is None:
        return {"ok": False, "message": f"No known cartridge named '{filename}'.", **status_data(db)}
    slot = db.execute("SELECT rom_path FROM cartridge.cartridge_slot WHERE id=1").fetchone()
    if slot and slot["rom_path"] and Path(slot["rom_path"]).name.casefold() == filename.casefold():
        return {"ok": False, "message": "That cartridge is inserted. Eject it before moving it.", **status_data(db)}
    db.execute("UPDATE cartridge_inventory SET location=?,updated_at=? WHERE filename=? COLLATE NOCASE", (destination, utc_now(), filename))
    db.commit()
    record_event(db, "physical", "cartridge_moved", f"R2 put the {current['title']} cartridge in {destination}.", f"Filename={filename}; destination={destination}.", current["title"])
    return {"ok": True, "message": f"Moved {current['title']} cartridge to {destination}.", **status_data(db)}


def status_data(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    sync_cartridge_inventory(db)
    return {
        "device": "Game Boy Advance",
        "power_state": row["power_state"],
        "cartridge_inserted": bool(row["cartridge_path"]),
        "cartridge_path": row["cartridge_path"],
        "cartridge_title": row["cartridge_title"],
        "emulator_pid": row["emulator_pid"],
        "game_running": bool(row["power_state"] == "on" and row["emulator_pid"]),
        "database": str(DB_PATH),
        "state_database": str(STATE_DB_PATH),
        "cartridges_directory": str(CARTRIDGES),
        "saves_directory": str(SAVES),
    }


def mgba_environment() -> dict[str, str]:
    env = os.environ.copy()
    # Isolate mGBA settings from the desktop user's personal emulator config.
    env["XDG_CONFIG_HOME"] = str(STATE / "mgba-config")
    config_dir = STATE / "mgba-config" / "mgba"
    config_dir.mkdir(parents=True, exist_ok=True)
    # Save paths and rewind/autosave options are passed explicitly with -C when
    # launching mGBA. Do not rewrite a config.ini with uncertain section state.
    # mGBA's Qt frontend reads keyboard shortcuts from qt.ini. Empty values
    # override its default save/load-state and rewind hotkeys for this console.
    qt_config = config_dir / "qt.ini"
    disabled_actions = [
        "loadState", "saveState", "loadStateFile", "saveStateFile",
        "quickLoad", "quickSave", "undoLoadState", "undoSaveState",
        "holdRewind", "rewind", "frameRewind",
    ]
    disabled_actions += [f"quickLoad.{slot}" for slot in range(1, 10)]
    disabled_actions += [f"quickSave.{slot}" for slot in range(1, 10)]
    qt_config.write_text(
        "[shortcutKey]\n" + "".join(f"{action}=\n" for action in disabled_actions),
        encoding="utf-8",
    )
    return env



def verify_console(db: sqlite3.Connection) -> dict[str, Any]:
    """Verify the installed device, current cartridge slot, and emulator path.

    This does not change power state or replace the cartridge. Power-on
    performs the actual launch using the single ROM stored in the slot.
    """
    row = reconcile(db)
    emulator = Path(EMULATOR).expanduser()
    if not emulator.is_absolute():
        emulator = emulator.resolve()
    checks = {
        "device_root_exists": ROOT.is_dir(),
        "cartridges_directory_exists": CARTRIDGES.is_dir(),
        "saves_directory_writable": SAVES.is_dir() and os.access(SAVES, os.W_OK),
        "state_directory_writable": STATE.is_dir() and os.access(STATE, os.W_OK),
        "emulator_exists": emulator.is_file(),
        "emulator_executable": emulator.is_file() and os.access(emulator, os.X_OK),
    }
    rom_path = row["cartridge_path"]
    if rom_path:
        rom = Path(rom_path).resolve()
        checks["slot_rom_exists"] = rom.is_file() and rom.parent == CARTRIDGES.resolve()
        checks["slot_rom_format_supported"] = rom.suffix.lower() in ROM_EXTENSIONS
    else:
        checks["slot_rom_exists"] = False
        checks["slot_rom_format_supported"] = False
    failures = []
    for key, description in (
        ("device_root_exists", "device root is missing"),
        ("cartridges_directory_exists", "Cartridges directory is missing"),
        ("saves_directory_writable", "Saves directory is not writable"),
        ("state_directory_writable", "State directory is not writable"),
        ("emulator_executable", f"mGBA is missing or not executable at {emulator}"),
    ):
        if not checks[key]:
            failures.append(description)
    if rom_path:
        if not checks["slot_rom_exists"]:
            failures.append("the recorded cartridge path is missing or outside Cartridges")
        if not checks["slot_rom_format_supported"]:
            failures.append("the inserted cartridge format is unsupported")
    ok = not failures
    if failures:
        message = "Console verification failed: " + "; ".join(failures) + "."
    elif not rom_path:
        message = "Console verified; cartridge slot is empty, so power on will not launch a game."
    else:
        message = f"Console verified. Power on will launch {row['cartridge_title'] or Path(rom_path).stem} from the current cartridge slot."
    return {
        "ok": ok, "message": message, "checks": checks,
        "emulator_path": str(emulator), **status_data(db),
    }

def power_on(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] == "on":
        return {"ok": False, "message": "The console is already powered on.", **status_data(db)}
    cartridge = row["cartridge_path"]
    if not cartridge:
        db.execute(
            "UPDATE console_state SET power_state='on', emulator_pid=NULL, "
            "emulator_start_ticks=NULL, game_started_at=NULL, session_id=NULL WHERE id=1"
        )
        db.commit()
        record_event(db, "physical", "console_powered_on_empty",
                     "R2 powered on the Game Boy Advance with no cartridge inserted.")
        return {"ok": True, "message": "Console powered on. The cartridge slot is empty; no emulator was launched.",
                **status_data(db)}
    rom = Path(cartridge).resolve()
    try:
        if rom.parent != CARTRIDGES.resolve() or not rom.is_file():
            raise ValueError("The inserted cartridge file is missing or outside Cartridges.")
        if rom.suffix.lower() not in ROM_EXTENSIONS:
            raise ValueError("The inserted cartridge format is unsupported.")
        emulator = Path(EMULATOR).expanduser().resolve()
        if not emulator.is_file() or not os.access(emulator, os.X_OK):
            raise ValueError(
                f"mGBA executable not found or not executable at {emulator}. "
                "Set R2_MGBA_EXECUTABLE to its actual executable path."
            )
        title = row["cartridge_title"] or read_rom_title(rom)
        session_id = f"gba-{int(time.time())}-{os.getpid()}"
        # Do not pass --savestate/-t, and do not expose a save-state command.
        # Use per-console config and only the one ROM recorded in the slot.
        proc = subprocess.Popen(
            [str(emulator), "-C", f"savegamePath={SAVES}",
             "-C", f"savestatePath={STATE / 'disabled-save-states'}",
             "-C", "autosave=false", "-C", "rewindEnable=false", str(rom)],
            cwd=str(ROOT), env=mgba_environment(),
            stdin=subprocess.DEVNULL, start_new_session=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(0.15)
        if proc.poll() is not None:
            return {"ok": False, "message": f"mGBA exited immediately with status {proc.returncode}.",
                    **status_data(db)}
        ticks = process_start_ticks(proc.pid)
        db.execute(
            "UPDATE console_state SET power_state='on', emulator_pid=?, emulator_start_ticks=?, "
            "game_started_at=?, session_id=? WHERE id=1",
            (proc.pid, ticks, utc_now(), session_id),
        )
        db.commit()
        record_event(db, "physical", "console_powered_on",
                     f"R2 powered on the Game Boy Advance with {title} inserted.",
                     "The console launched mGBA with only the ROM recorded in the cartridge slot.",
                     title, session_id)
        record_event(db, "virtual", "game_session_started",
                     f"An emulated game session began: {title}.",
                     "This is an in-game context; it is not a claim that game events are physical-world events.",
                     title, session_id)
        return {"ok": True, "message": f"Console powered on; started {title}.",
                **status_data(db)}
    except (OSError, ValueError) as exc:
        return {"ok": False, "message": str(exc), **status_data(db)}


def power_off(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] == "off":
        return {"ok": True, "message": "The console is already powered off.", **status_data(db)}
    pid = row["emulator_pid"]
    title = row["cartridge_title"] or "unknown game"
    session_id = row["session_id"]
    if pid and process_matches(int(pid), row["emulator_start_ticks"]):
        try:
            # Ask the window manager to close mGBA normally first. This lets
            # the emulator flush already-written cartridge save RAM to its
            # .sav backing file; it does not trigger an in-game save command.
            if shutil.which("xdotool"):
                found = subprocess.run(
                    ["xdotool", "search", "--onlyvisible", "--pid", str(pid)],
                    capture_output=True, text=True, timeout=2, check=False,
                )
                windows = [line.strip() for line in found.stdout.splitlines() if line.strip()]
                if windows:
                    subprocess.run(
                        ["xdotool", "windowclose", windows[0]],
                        check=False, timeout=3,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
            deadline = time.monotonic() + 4.0
            while time.monotonic() < deadline and process_matches(
                int(pid), row["emulator_start_ticks"]
            ):
                time.sleep(0.1)
            # Fall back only if the emulator did not close in response.
            if process_matches(int(pid), row["emulator_start_ticks"]):
                os.kill(int(pid), signal.SIGTERM)
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline and process_matches(
                    int(pid), row["emulator_start_ticks"]
                ):
                    time.sleep(0.1)
            if process_matches(int(pid), row["emulator_start_ticks"]):
                os.kill(int(pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        except PermissionError:
            return {"ok": False, "message": "Permission denied while powering off the emulator.",
                    **status_data(db)}
    db.execute(
        "UPDATE console_state SET power_state='off', emulator_pid=NULL, "
        "emulator_start_ticks=NULL, game_started_at=NULL, session_id=NULL WHERE id=1"
    )
    db.commit()
    record_event(
        db, "physical", "console_powered_off",
        "R2 powered off the Game Boy Advance.",
        "Power-off does not create a save state or invoke an in-game save. Unsaved progress may be lost.",
        title, session_id,
    )
    if session_id:
        record_event(db, "virtual", "game_session_interrupted",
                     f"The emulated session for {title} ended when the console was powered off.",
                     "The console did not issue an in-game save command.",
                     title, session_id)
    return {"ok": True, "message": "Console powered off. No in-game save command or save state was issued.",
            **status_data(db)}


def insert_cartridge(db: sqlite3.Connection, name: str) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] != "off":
        return {"ok": False,
                "message": "The Game Boy Advance is powered on. Turn it off before removing or replacing the cartridge.",
                **status_data(db)}
    if row["cartridge_path"]:
        return {"ok": False, "message": "A cartridge is already inserted. Eject it before inserting another game.", **status_data(db)}
    try:
        path = safe_cartridge(name)
    except (ValueError, OSError) as exc:
        return {"ok": False, "message": str(exc), **status_data(db)}
    sync_cartridge_inventory(db)
    inventory = db.execute("SELECT location FROM cartridge_inventory WHERE filename=? COLLATE NOCASE", (path.name,)).fetchone()
    if inventory is None:
        return {"ok": False, "message": "Cartridge is not registered in R2's inventory.", **status_data(db)}
    if inventory["location"] != "pockets":
        return {"ok": False, "message": f"The {read_rom_title(path)} cartridge is in {inventory['location']}, not R2's pockets. Retrieve it first.", **status_data(db)}
    title = read_rom_title(path)
    db.execute(
        "UPDATE cartridge.cartridge_slot SET rom_path=?, rom_title=?, inserted_at=? WHERE id=1",
        (str(path), title, utc_now()),
    )
    db.execute("UPDATE cartridge_inventory SET location='slot',updated_at=? WHERE filename=? COLLATE NOCASE", (utc_now(), path.name))
    db.commit()
    record_event(db, "physical", "cartridge_inserted",
                 f"R2 inserted the {title} cartridge.",
                 f"ROM filename: {path.name}", title)
    return {"ok": True, "message": f"Inserted cartridge: {title}.",
            **status_data(db)}


def eject_cartridge(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] != "off":
        return {"ok": False,
                "message": "The Game Boy Advance is powered on. Turn it off before removing or replacing the cartridge.",
                **status_data(db)}
    title = row["cartridge_title"]
    if not row["cartridge_path"]:
        return {"ok": False, "message": "The cartridge slot is already empty.", **status_data(db)}
    filename = Path(row["cartridge_path"]).name
    db.execute("UPDATE cartridge.cartridge_slot SET rom_path=NULL, rom_title=NULL, inserted_at=NULL WHERE id=1")
    db.execute("UPDATE cartridge_inventory SET location='pockets',updated_at=? WHERE filename=? COLLATE NOCASE", (utc_now(), filename))
    db.commit()
    record_event(db, "physical", "cartridge_ejected", f"R2 removed the {title} cartridge and is carrying it in his pockets.", f"Filename={filename}; destination=pockets.", title)
    return {"ok": True, "message": f"Ejected {title}. The cartridge slot is now empty.",
            **status_data(db)}


def list_cartridges(db: sqlite3.Connection) -> list[dict[str, Any]]:
    sync_cartridge_inventory(db)
    rows = db.execute("SELECT filename,title,location FROM cartridge_inventory ORDER BY filename COLLATE NOCASE").fetchall()
    return [{"filename": row["filename"], "title": row["title"], "location": row["location"], "file_available": (CARTRIDGES / row["filename"]).is_file()} for row in rows]


def press_button(db: sqlite3.Connection, button: str, duration_ms: int) -> dict[str, Any]:
    row = reconcile(db)
    name = button.upper()
    if name not in BUTTON_KEYS:
        return {"ok": False, "message": "Unknown GBA button. Use A, B, L, R, START, SELECT, UP, DOWN, LEFT, or RIGHT.",
                **status_data(db)}
    if row["power_state"] != "on" or not row["emulator_pid"]:
        return {"ok": False, "message": "No game is running. Power on the console with a cartridge inserted first.",
                **status_data(db)}
    if duration_ms < 1 or duration_ms > MAX_PRESS_MS:
        return {"ok": False, "message": f"Button duration must be 1–{MAX_PRESS_MS} milliseconds.",
                **status_data(db)}
    if shutil.which("xdotool") is None:
        return {"ok": False, "message": "xdotool is not installed; controller input was not sent. Install xdotool to enable R2 button presses.",
                **status_data(db)}
    pid = str(row["emulator_pid"])
    try:
        found = subprocess.run(
            ["xdotool", "search", "--onlyvisible", "--pid", pid],
            capture_output=True, text=True, timeout=2, check=False,
        )
        windows = [line.strip() for line in found.stdout.splitlines() if line.strip()]
        if found.returncode != 0 or not windows:
            return {"ok": False, "message": "Could not find the running mGBA window; no button press was sent.",
                    **status_data(db)}
        window = windows[0]
        key = BUTTON_KEYS[name]
        subprocess.run(["xdotool", "windowactivate", "--sync", window],
                       check=True, timeout=3, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run(["xdotool", "keydown", "--window", window, key],
                       check=True, timeout=3, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        time.sleep(duration_ms / 1000.0)
        subprocess.run(["xdotool", "keyup", "--window", window, key],
                       check=True, timeout=3, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except (OSError, subprocess.SubprocessError) as exc:
        return {"ok": False, "message": f"Button input failed: {exc}", **status_data(db)}
    title = row["cartridge_title"] or "unknown game"
    record_event(db, "physical", "controller_input",
                 f"R2 pressed {name} while playing {title}.",
                 f"Emulator key={BUTTON_KEYS[name]}; duration_ms={duration_ms}.",
                 title, row["session_id"])
    return {"ok": True, "message": f"Sent {name} button press for {duration_ms} ms.",
            **status_data(db)}


def print_result(value: Any, as_json: bool) -> None:
    if as_json:
        print(json.dumps(value, ensure_ascii=False))
        return
    if isinstance(value, dict):
        print(value.get("message", ""))
        if "power_state" in value:
            print(f"Power: {value['power_state']}")
            print(f"Cartridge: {value.get('cartridge_title') or '(empty)'}")
            print(f"Game running: {'yes' if value.get('game_running') else 'no'}")
            print(f"Database: {value.get('database')}")
            if "checks" in value:
                for check, passed in value["checks"].items():
                    print(f"Check {check}: {'OK' if passed else 'FAILED'}")
                print(f"mGBA: {value.get('emulator_path')}")
    elif isinstance(value, list):
        if not value:
            print("No cartridges or pending verified game events.")
        for item in value:
            if "filename" in item:
                print(f"{item['filename']} — {item['title']} [{item.get('location', 'unknown location')}]")
            else:
                print(f"#{item['event_id']} [{item['context']}] {item['summary']} "
                      f"({item['evidence_source']})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="R2's persistent virtual Game Boy Advance console")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("status", help="inspect console and cartridge state")
    sub.add_parser("verify", help="verify install, emulator, and currently inserted ROM before power-on")
    sub.add_parser("list", help="list known cartridges and their physical locations")
    move = sub.add_parser("move", help="move a loose cartridge to pockets, shelf, box, or room")
    move.add_argument("filename")
    move.add_argument("location", choices=("pockets", "shelf", "box", "room"))
    ins = sub.add_parser("insert", help="insert a cartridge while powered off")
    ins.add_argument("filename")
    sub.add_parser("eject", help="eject the cartridge while powered off")
    power = sub.add_parser("power", help="power on or off")
    power.add_argument("state", choices=("on", "off"))
    press = sub.add_parser("press", help="send one GBA controller button press")
    press.add_argument("button")
    press.add_argument("duration_ms", nargs="?", type=int, default=DEFAULT_PRESS_MS)
    sub.add_parser("events", help="list verified virtual events waiting for Life Log sync")
    ack = sub.add_parser("ack_event", help="acknowledge a verified event after Life Log sync")
    ack.add_argument("event_id", type=int)
    args = parser.parse_args(argv)
    command = args.command or "power"
    state = args.state if args.command == "power" else "on"
    db = connect()
    try:
        if command == "status":
            result = status_data(db)
        elif command == "verify":
            result = verify_console(db)
        elif command == "list":
            result = list_cartridges(db)
        elif command == "move":
            result = move_cartridge(db, args.filename, args.location)
        elif command == "insert":
            result = insert_cartridge(db, args.filename)
        elif command == "eject":
            result = eject_cartridge(db)
        elif command == "press":
            result = press_button(db, args.button, args.duration_ms)
        elif command == "events":
            result = pending_verified_events(db)
        elif command == "ack_event":
            result = {"ok": acknowledge_verified_event(db, args.event_id),
                      "message": "Verified event acknowledged."}
        elif command == "power":
            result = power_on(db) if state == "on" else power_off(db)
        else:
            result = {"ok": False, "message": f"Unsupported command: {command}"}
        print_result(result, args.json)
        return 0 if not isinstance(result, dict) or result.get("ok", True) else 1
    finally:
        db.close()


if __name__ == "__main__":
    raise SystemExit(main())
