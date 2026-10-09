#!/usr/bin/env python3
"""Reality-backed CRT display and built-in VCR renderer for R2-3PO.

Reality remains authoritative for TV controls, inserted tape, transport state,
and per-file playback position. This GUI reads SQLite read-only and sends all
state changes to the running R2 shell over its private Unix socket.
"""
from __future__ import annotations

import os
import socket
import sqlite3
import time
import tkinter as tk
from tkinter import filedialog
from pathlib import Path

DB_PATH = Path(os.environ.get("R2_REALITY_DB", "/home/x/R2_Home/R2/r2_reality.db"))
SOCKET_PATH = Path(os.environ.get("R2_TV_SOCKET", "/home/x/R2_Home/R2/tv-control.sock"))
MEDIA_DIR = Path(os.environ.get("R2_VCR_MEDIA_DIR", "/home/x/R2_Home/VCR_Tapes"))
POLL_MS = 1000
POSITION_SAVE_SECONDS = 5
MEDIA_EXTENSIONS = (
    ".avi", ".mkv", ".mp4", ".m4v", ".mov", ".mpeg", ".mpg", ".wmv",
    ".webm", ".ogv", ".flv", ".ts", ".vob", ".3gp", ".asf", ".m2ts",
    ".mts",
)


def send_control_command(command: str) -> str:
    """Send a bounded request to R2; C routes it through Reality APIs."""
    allowed = {"power on", "power off", "vcr play", "vcr pause", "vcr stop", "vcr eject"}
    valid = command in allowed
    valid = valid or (
        command.startswith("input ") and command[6:].isdigit()
        and 1 <= int(command[6:]) <= 4
    )
    valid = valid or (
        command.startswith("tune ") and command[5:].isdigit()
        and 2 <= int(command[5:]) <= 13
    )
    if command.startswith("vcr insert "):
        path = command[11:]
        valid = Path(path).is_absolute() and "\n" not in path and "\r" not in path and len(command) <= 1199
    if command.startswith("vcr position "):
        try:
            seconds = float(command[13:])
            valid = 0 <= seconds <= 604800
        except ValueError:
            valid = False
    if not valid:
        raise ValueError("Unsupported TV/VCR control command")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(2.0)
        client.connect(str(SOCKET_PATH))
        client.sendall((command + "\n").encode("utf-8"))
        response = client.recv(8192).decode("utf-8", errors="replace")
    if not response.startswith("OK"):
        raise RuntimeError(response or "R2 returned no response")
    return response


class CRTDisplay:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("R2-3PO — CRT Television")
        root.geometry("760x700")
        root.minsize(560, 590)
        root.configure(bg="#242424")

        bezel = tk.Frame(root, bg="#343434", padx=26, pady=24,
                         highlightthickness=2, highlightbackground="#555555")
        bezel.pack(fill="both", expand=True, padx=18, pady=18)

        self.screen = tk.Frame(bezel, bg="#080b08", highlightthickness=5,
                               highlightbackground="#111111")
        self.screen.pack(fill="both", expand=True)
        self.video_surface = tk.Frame(self.screen, bg="#080b08")
        self.video_surface.place(relx=0, rely=0, relwidth=1, relheight=1)
        self.picture = tk.Label(
            self.screen, text="TV OFF", bg="#080b08", fg="#a6b99c",
            font=("DejaVu Sans Mono", 24, "bold"), justify="center",
        )
        self.picture.place(relx=0, rely=0, relwidth=1, relheight=1)
        self.video_surface.lower()

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

        vcr_row = tk.Frame(controls, bg="#242424")
        vcr_row.pack(fill="x", pady=(5, 0))
        tk.Label(vcr_row, text="BUILT-IN VCR", bg="#242424", fg="#dddddd").pack(side="left", padx=(0, 8))
        self._button(vcr_row, "LOAD TAPE", self.load_tape).pack(side="left", padx=2)
        self._button(vcr_row, "PLAY", lambda: self.control("vcr play")).pack(side="left", padx=2)
        self._button(vcr_row, "PAUSE", lambda: self.control("vcr pause")).pack(side="left", padx=2)
        self._button(vcr_row, "STOP", lambda: self.control("vcr stop")).pack(side="left", padx=2)
        self._button(vcr_row, "EJECT", lambda: self.control("vcr eject")).pack(side="left", padx=2)

        self.feedback = tk.Label(
            controls, text="Controls require R2 to be running.", anchor="w",
            bg="#242424", fg="#dddddd", font=("DejaVu Sans Mono", 9),
        )
        self.feedback.pack(fill="x", pady=(6, 0))

        self._vlc = None
        self._vlc_instance = None
        self._player = None
        self._loaded_path: str | None = None
        self._last_transport: str | None = None
        self._last_position_save = time.monotonic()
        self._vlc_error_shown = False
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh()

    @staticmethod
    def _button(parent: tk.Widget, text: str, command) -> tk.Button:
        return tk.Button(parent, text=text, command=command, bg="#444444", fg="#eeeeee",
                         activebackground="#606060", activeforeground="#ffffff",
                         relief="raised", padx=8, pady=3)

    def control(self, command: str) -> None:
        try:
            # Save the outgoing tape before transport changes or inserting another.
            if command.startswith("vcr ") and not command.startswith("vcr position "):
                self._save_vcr_position(force=True)
            response = send_control_command(command)
            self.feedback.configure(text=response, fg="#a6b99c")
            self.refresh(schedule=False)
        except (OSError, RuntimeError, ValueError) as exc:
            self.feedback.configure(text=f"Control unavailable: {exc}", fg="#e0b0a0")

    def load_tape(self) -> None:
        path = filedialog.askopenfilename(
            parent=self.root, title="Insert a video/audio tape",
            initialdir=str(MEDIA_DIR if MEDIA_DIR.is_dir() else Path.home()),
            filetypes=[("VLC media", " ".join(f"*{ext}" for ext in MEDIA_EXTENSIONS)),
                       ("All files", "*.*")],
        )
        if path:
            self.control("vcr insert " + str(Path(path).resolve()))

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

    def read_vcr_state(self) -> tuple[str | None, str, float]:
        uri = f"file:{DB_PATH}?mode=ro"
        with sqlite3.connect(uri, uri=True, timeout=1.5) as db:
            row = db.execute(
                "SELECT cassette_path, transport FROM r2_tv_vcr_state WHERE id=1"
            ).fetchone()
            if row is None:
                raise RuntimeError("VCR state row is missing")
            path = str(row[0]) if row[0] else None
            transport = str(row[1])
            position = 0.0
            if path:
                tape = db.execute(
                    "SELECT position_seconds FROM r2_tv_vcr_tapes WHERE media_path=?",
                    (path,),
                ).fetchone()
                if tape:
                    position = max(0.0, float(tape[0]))
            return path, transport, position

    def _ensure_player(self) -> bool:
        if self._player is not None:
            return True
        try:
            import vlc
            self._vlc = vlc
            self._vlc_instance = vlc.Instance("--no-video-title-show", "--quiet")
            if self._vlc_instance is None:
                raise RuntimeError("VLC could not initialize")
            self._player = self._vlc_instance.media_player_new()
            self.root.update_idletasks()
            self._player.set_xwindow(self.video_surface.winfo_id())
            return True
        except (ImportError, OSError, RuntimeError) as exc:
            if not self._vlc_error_shown:
                self.feedback.configure(
                    text=f"VCR playback needs VLC and python3-vlc: {exc}", fg="#e0b0a0"
                )
                self._vlc_error_shown = True
            return False

    def _prepare_media(self, path: str, position: float, transport: str) -> None:
        if not self._ensure_player():
            return
        media = self._vlc_instance.media_new(path)
        self._player.set_media(media)
        self._loaded_path = path
        self._last_transport = None
        self._player.play()

        def seek_and_apply() -> None:
            if self._player is None or self._loaded_path != path:
                return
            self._player.set_time(int(position * 1000))
            if transport == "play":
                self._player.set_pause(0)
            else:
                self._player.set_pause(1)
            self._last_transport = transport

        self.root.after(500, seek_and_apply)

    def _save_vcr_position(self, force: bool = False) -> None:
        if not self._player or not self._loaded_path or self._last_transport != "play":
            return
        now = time.monotonic()
        if not force and now - self._last_position_save < POSITION_SAVE_SECONDS:
            return
        milliseconds = self._player.get_time()
        if milliseconds >= 0:
            try:
                send_control_command(f"vcr position {milliseconds / 1000:.3f}")
                self._last_position_save = now
            except (OSError, RuntimeError, ValueError):
                pass

    def sync_vcr_player(self, path: str | None, transport: str, position: float) -> None:
        if path != self._loaded_path:
            if path is None:
                if self._player:
                    self._player.stop()
                self._loaded_path = None
                self._last_transport = None
            else:
                self._prepare_media(path, position, transport)
        elif path and self._player and transport != self._last_transport:
            if transport == "play":
                if self._player.play() < 0:
                    self._player.set_pause(0)
            else:
                self._player.set_pause(1)
            self._last_transport = transport
        self._save_vcr_position()

    def close(self) -> None:
        self._save_vcr_position(force=True)
        if self._player:
            self._player.stop()
        self.root.destroy()

    def refresh(self, schedule: bool = True) -> None:
        try:
            power, kind, value, device = self.read_state()
            tape_path, transport, position = self.read_vcr_state()
            self.sync_vcr_player(tape_path, transport, position)

            if not power:
                headline = "TV OFF"
                source = "Standby"
            elif kind == "rf":
                source = f"RF CHANNEL {value}"
                headline = device or "NO SIGNAL"
            elif value == 1:
                source = "AV INPUT 1 — BUILT-IN VCR"
                if not tape_path:
                    headline = "VCR EMPTY\nNO TAPE INSERTED"
                elif transport == "play":
                    headline = ""
                elif transport == "pause":
                    headline = "VCR PAUSED"
                else:
                    headline = "VCR STOPPED"
            else:
                source = f"AV INPUT {value}"
                headline = device or "NO SIGNAL"

            showing_vcr = bool(power and kind == "input" and value == 1 and tape_path
                               and transport == "play" and self._player is not None)
            if showing_vcr:
                self.video_surface.lift()
            else:
                self.picture.lift()
                self.picture.configure(
                    text=headline,
                    fg="#a6b99c" if power and device else "#b8c0b2",
                    bg="#080b08" if power else "#050605",
                )

            tape_name = Path(tape_path).name if tape_path else "no tape"
            self.status.configure(
                text=f"{'POWER ON' if power else 'POWER OFF'}   |   {source}"
                     + (f"   |   {device}" if device and value != 1 else "")
                     + f"   |   VCR: {tape_name} [{transport}]"
                     + "   |   Reality-linked controls"
            )
        except (sqlite3.Error, OSError, RuntimeError) as exc:
            self.picture.lift()
            self.picture.configure(text="TV STATE UNAVAILABLE", fg="#b8c0b2")
            self.status.configure(text=f"Database: {DB_PATH}   |   {exc}")
        if schedule:
            self.root.after(POLL_MS, self.refresh)


def main() -> None:
    root = tk.Tk()
    CRTDisplay(root)
    root.mainloop()


if __name__ == "__main__":
    main()
