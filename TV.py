#!/usr/bin/env python3
"""Read-only CRT TV display for R2-3PO.

This first UI step mirrors the authoritative Reality database. It deliberately
does not write TV state or create an RF signal; controls will be connected
through the existing Reality/event APIs in a later step.
"""
from __future__ import annotations

import os
import sqlite3
import tkinter as tk
from pathlib import Path

DB_PATH = Path(os.environ.get("R2_REALITY_DB", "/home/x/R2_Home/R2/r2_reality.db"))
POLL_MS = 1000


class CRTDisplay:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("R2-3PO — CRT Television")
        root.geometry("760x520")
        root.minsize(520, 380)
        root.configure(bg="#242424")

        bezel = tk.Frame(root, bg="#343434", padx=26, pady=24,
                         highlightthickness=2, highlightbackground="#555555")
        bezel.pack(fill="both", expand=True, padx=18, pady=18)

        self.screen = tk.Frame(bezel, bg="#080b08", highlightthickness=5,
                               highlightbackground="#111111")
        self.screen.pack(fill="both", expand=True)

        self.picture = tk.Label(
            self.screen, text="TV OFF", bg="#080b08", fg="#a6b99c",
            font=("DejaVu Sans Mono", 24, "bold"), justify="center",
        )
        self.picture.pack(fill="both", expand=True, padx=12, pady=12)

        self.status = tk.Label(
            root, text="Reading persistent TV state…", anchor="w",
            bg="#242424", fg="#dddddd", font=("DejaVu Sans Mono", 10),
            padx=24, pady=4,
        )
        self.status.pack(fill="x")
        self.refresh()

    def read_state(self) -> tuple[int, str, int, str | None]:
        uri = f"file:{DB_PATH}?mode=ro"
        with sqlite3.connect(uri, uri=True, timeout=1.5) as db:
            row = db.execute(
                "SELECT power, source_kind, source_value FROM r2_tv_state WHERE id=1"
            ).fetchone()
            if row is None:
                raise RuntimeError("TV state row is missing")
            power, kind, value = int(row[0]), str(row[1]), int(row[2])
            device = None
            if kind == "rf":
                match = db.execute(
                    "SELECT name FROM r2_tv_devices "
                    "WHERE connected=1 AND connection_kind='rf' AND port=? LIMIT 1",
                    (value,),
                ).fetchone()
                device = str(match[0]) if match else None
            elif kind == "input" and value > 1:
                match = db.execute(
                    "SELECT name FROM r2_tv_devices "
                    "WHERE connected=1 AND connection_kind='input' AND port=? LIMIT 1",
                    (value,),
                ).fetchone()
                device = str(match[0]) if match else None
            return power, kind, value, device

    def refresh(self) -> None:
        try:
            power, kind, value, device = self.read_state()
            if not power:
                headline = "TV OFF"
                source = "Standby"
            else:
                if kind == "rf":
                    source = f"RF CHANNEL {value}"
                    headline = device or "NO SIGNAL"
                elif value == 1:
                    source = "AV INPUT 1 — BUILT-IN VCR"
                    headline = "VCR INPUT\nPlayback not connected yet"
                else:
                    source = f"AV INPUT {value}"
                    headline = device or "NO SIGNAL"
            self.picture.configure(
                text=headline,
                fg="#a6b99c" if power and device else "#b8c0b2",
                bg="#080b08" if power else "#050605",
            )
            self.status.configure(
                text=f"{'POWER ON' if power else 'POWER OFF'}   |   {source}"
                     + (f"   |   {device}" if device and value != 1 else "")
                     + "   |   Read-only display"
            )
        except (sqlite3.Error, OSError, RuntimeError) as exc:
            self.picture.configure(text="TV STATE UNAVAILABLE", fg="#b8c0b2")
            self.status.configure(text=f"Database: {DB_PATH}   |   {exc}")
        self.root.after(POLL_MS, self.refresh)


def main() -> None:
    root = tk.Tk()
    CRTDisplay(root)
    root.mainloop()


if __name__ == "__main__":
    main()
