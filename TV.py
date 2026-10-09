#!/usr/bin/env python3
"""CRT TV display for R2-3PO, with controls routed through the live R2 process.

The display reads Reality state read-only. Control requests use a local Unix
socket served by shell.c and call the existing Reality APIs, preserving event
bridging. The GUI never writes the Reality database directly.
"""
from __future__ import annotations

import os
import socket
import sqlite3
import tkinter as tk
from pathlib import Path

DB_PATH = Path(os.environ.get("R2_REALITY_DB", "/home/x/R2_Home/R2/r2_reality.db"))
SOCKET_PATH = Path(os.environ.get("R2_TV_SOCKET", "/home/x/R2_Home/R2/tv-control.sock"))
POLL_MS = 1000


def send_control_command(command: str) -> str:
    """Send a bounded request to R2; the C server routes it through Reality APIs."""
    allowed = {"power on", "power off"}
    if command not in allowed and not (
        command.startswith("input ") and command[6:].isdigit() and 1 <= int(command[6:]) <= 4
    ) and not (
        command.startswith("tune ") and command[5:].isdigit() and 2 <= int(command[5:]) <= 13
    ):
        raise ValueError("Unsupported TV control command")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(2.0)
        client.connect(str(SOCKET_PATH))
        client.sendall((command + "\\n").encode("utf-8"))
        response = client.recv(8192).decode("utf-8", errors="replace")
    if not response.startswith("OK"):
        raise RuntimeError(response or "R2 returned no response")
    return response


class CRTDisplay:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("R2-3PO — CRT Television")
        root.geometry("760x620")
        root.minsize(560, 500)
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

        controls = tk.Frame(root, bg="#242424", padx=18, pady=8)
        controls.pack(fill="x")
        power_row = tk.Frame(controls, bg="#242424")
        power_row.pack(fill="x", pady=(0, 5))
        self._button(power_row, "POWER ON", lambda: self.control("power on")).pack(side="left", padx=(0, 6))
        self._button(power_row, "POWER OFF", lambda: self.control("power off")).pack(side="left")

        input_row = tk.Frame(controls, bg="#242424")
        input_row.pack(fill="x", pady=(0, 5))
        tk.Label(input_row, text="AV INPUT", bg="#242424", fg="#dddddd").pack(side="left", padx=(0, 8))
        for number in range(1, 5):
            self._button(input_row, str(number), lambda n=number: self.control(f"input {n}")).pack(side="left", padx=2)

        rf_row = tk.Frame(controls, bg="#242424")
        rf_row.pack(fill="x")
        tk.Label(rf_row, text="RF CHANNEL", bg="#242424", fg="#dddddd").pack(side="left", padx=(0, 8))
        self.rf_channel = tk.Spinbox(rf_row, from_=2, to=13, width=4, font=("DejaVu Sans Mono", 10))
        self.rf_channel.pack(side="left", padx=(0, 8))
        self._button(rf_row, "TUNE", self.tune_rf).pack(side="left")
        self.feedback = tk.Label(controls, text="Controls require R2 to be running.", anchor="w",
                                 bg="#242424", fg="#dddddd", font=("DejaVu Sans Mono", 9))
        self.feedback.pack(fill="x", pady=(6, 0))
        self.refresh()

    @staticmethod
    def _button(parent: tk.Widget, text: str, command) -> tk.Button:
        return tk.Button(parent, text=text, command=command, bg="#444444", fg="#eeeeee",
                         activebackground="#606060", activeforeground="#ffffff",
                         relief="raised", padx=10, pady=3)

    def control(self, command: str) -> None:
        try:
            response = send_control_command(command)
            self.feedback.configure(text=response, fg="#a6b99c")
            self.refresh()
        except (OSError, RuntimeError, ValueError) as exc:
            self.feedback.configure(text=f"Control unavailable: {exc}", fg="#e0b0a0")

    def tune_rf(self) -> None:
        try:
            channel = int(self.rf_channel.get())
        except ValueError:
            self.feedback.configure(text="Choose an RF channel from 2 to 13.", fg="#e0b0a0")
            return
        self.control(f"tune {channel}")

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
                     + "   |   Reality-linked controls"
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
