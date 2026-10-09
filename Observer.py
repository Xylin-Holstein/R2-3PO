#!/usr/bin/env python3
"""Read-only desktop observer for R2's factual public event summaries."""
from __future__ import annotations
import os
import sqlite3
import time
from pathlib import Path
DATABASE = Path(os.environ.get("R2_OBSERVER_DATABASE", "/home/x/R2_Home/R2/r2_memory.db"))
POLL_MS = 900
MAX_ROWS = 250
# New event types remain hidden unless explicitly reviewed and added here.
PUBLIC_EVENT_TYPES = frozenset({
    "object_added", "object_moved", "object_removed", "fridge_item_taken",
    "fridge_food_consumed", "fridge_item_stored", "food_consumed", "purchase",
    # Generic future hooks for verified activity sessions and location changes.
    "activity_started", "activity_progress", "activity_ended",
    "location_changed", "departure", "arrival",
})
def is_observer_event(category: str, event_type: str) -> bool:
    """True only for explicitly approved factual event types/categories."""
    return event_type in PUBLIC_EVENT_TYPES and category in {"world", "media"}
def open_database():
    # The window cannot create or mutate the private Life Log database.
    uri = DATABASE.resolve().as_uri() + "?mode=ro"
    connection = sqlite3.connect(uri, uri=True, timeout=2.0)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA query_only=ON")
    return connection
def main() -> int:
    try:
        import tkinter as tk
        from tkinter import ttk
    except ImportError:
        print("[R2 Observer] Tkinter is unavailable; install python3-tk to use the popup.")
        return 2
    root = tk.Tk()
    root.title("R2-3PO — Observer Log")
    root.geometry("760x460")
    root.minsize(520, 280)
    header = ttk.Frame(root, padding=(12, 10, 12, 6))
    header.pack(fill="x")
    ttk.Label(header, text="R2-3PO", font=("TkDefaultFont", 14, "bold")).pack(side="left")
    ttk.Label(header, text="Factual activity timeline · private thoughts excluded",
              foreground="#666666").pack(side="right")
    body = ttk.Frame(root, padding=(10, 4, 10, 10))
    body.pack(fill="both", expand=True)
    tree = ttk.Treeview(body, columns=("time", "event"), show="headings")
    tree.heading("time", text="R2 time")
    tree.heading("event", text="Observed event")
    tree.column("time", width=155, minwidth=130, stretch=False)
    tree.column("event", width=560, minwidth=250, stretch=True)
    scroll = ttk.Scrollbar(body, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=scroll.set)
    tree.pack(side="left", fill="both", expand=True)
    scroll.pack(side="right", fill="y")
    status = tk.StringVar(value="Waiting for R2's Life Log…")
    ttk.Label(root, textvariable=status, anchor="w", padding=(12, 4)).pack(fill="x")
    last_id = 0
    connection = None
    last_open_attempt = 0.0
    allowed_types = tuple(sorted(PUBLIC_EVENT_TYPES))
    allowed_placeholders = ",".join("?" for _ in allowed_types)
    def poll():
        nonlocal last_id, connection, last_open_attempt
        try:
            if connection is None and time.monotonic() - last_open_attempt >= 2.0:
                last_open_attempt = time.monotonic()
                if DATABASE.exists():
                    try:
                        connection = open_database()
                    except sqlite3.Error:
                        connection = None
            if connection is not None:
                latest = connection.execute(
                    "SELECT COALESCE(MAX(id), 0) FROM r2_log_events"
                ).fetchone()[0]
                # Read only approved summaries; private text never enters the UI process.
                rows = connection.execute(
                    "SELECT id, local_time, category, event_type, summary "
                    "FROM r2_log_events WHERE id > ? AND id <= ? "
                    "AND category IN ('world','media') "
                    f"AND event_type IN ({allowed_placeholders}) ORDER BY id ASC",
                    (last_id, latest, *allowed_types),
                ).fetchall()
                for row in rows:
                    summary = " ".join((row["summary"] or "").split())
                    tree.insert("", "end", iid=str(row["id"]),
                                values=(row["local_time"] or "time unavailable", summary))
                last_id = int(latest)
                children = tree.get_children()
                if len(children) > MAX_ROWS:
                    for item in children[:len(children) - MAX_ROWS]:
                        tree.delete(item)
                if children:
                    tree.yview_moveto(1.0)
                status.set("Live · public factual events only")
        except sqlite3.Error:
            if connection is not None:
                try:
                    connection.close()
                except sqlite3.Error:
                    pass
                connection = None
            status.set("Life Log temporarily unavailable; retrying…")
        root.after(POLL_MS, poll)
    def close():
        if connection is not None:
            try:
                connection.close()
            except sqlite3.Error:
                pass
        root.destroy()
    root.protocol("WM_DELETE_WINDOW", close)
    root.after(100, poll)
    root.mainloop()
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
