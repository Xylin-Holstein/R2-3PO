#!/usr/bin/env python3
"""Display-only V-Webcam for R2's existing Eyes stream.

This window mirrors frames already captured by Eyes. It does not open a camera,
control VLC, or feed pixels back into perception.
"""
from __future__ import annotations

import fcntl
import json
import math
import os
from pathlib import Path
import tkinter as tk
from tkinter import ttk

DATA_DIR = Path("/tmp/r2-vwebcam-r2")
FRAME_PATH = DATA_DIR / "frame.ppm"
STATUS_PATH = DATA_DIR / "status.txt"
FOCUS_PATH = DATA_DIR / "focus.json"
LOCK_PATH = DATA_DIR / "viewer.lock"
BACKGROUND = "#111820"
PANEL = "#1a2530"
TEXT = "#e8f0f7"
MUTED = "#8fa3b7"
FOCUS_COLOR = "#42f58d"
WAITING_COLOR = "#ffbd59"


class VWebcam:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("R2-3PO | V-Webcam")
        self.root.configure(bg=BACKGROUND)
        self.root.geometry("1050x700")
        self.root.minsize(640, 420)
        self.paused = False
        self.show_focus = True
        self.photo: tk.PhotoImage | None = None
        self.raw_photo: tk.PhotoImage | None = None
        self.last_frame_mtime: int | None = None
        self.image_width = 0
        self.image_height = 0

        top = tk.Frame(root, bg=PANEL, padx=12, pady=9)
        top.pack(fill="x")
        tk.Label(
            top, text="R2-3PO / V-WEBCAM", bg=PANEL, fg=TEXT,
            font=("DejaVu Sans", 13, "bold"),
        ).pack(side="left")
        self.live_label = tk.Label(
            top, text="WAITING FOR EYES", bg=PANEL, fg=WAITING_COLOR,
            font=("DejaVu Sans", 9, "bold"),
        )
        self.live_label.pack(side="right")

        self.canvas = tk.Canvas(root, bg="#05080b", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=10, pady=(10, 4))
        self.image_item = self.canvas.create_image(0, 0, anchor="nw")
        self.focus_box = self.canvas.create_rectangle(
            0, 0, 0, 0, outline=FOCUS_COLOR, width=3, state="hidden",
        )
        self.focus_label = self.canvas.create_text(
            8, 8, anchor="nw", text="", fill=FOCUS_COLOR,
            font=("DejaVu Sans", 10, "bold"), state="hidden",
        )

        info = tk.Frame(root, bg=PANEL, padx=12, pady=8)
        info.pack(fill="x", padx=10, pady=(4, 8))
        self.source_label = tk.Label(
            info, text="Source: waiting", bg=PANEL, fg=TEXT,
            font=("DejaVu Sans", 10, "bold"), anchor="w",
        )
        self.source_label.pack(fill="x")
        self.details_label = tk.Label(
            info, text="No frame received yet.", bg=PANEL, fg=MUTED,
            font=("DejaVu Sans", 9), anchor="w",
        )
        self.details_label.pack(fill="x", pady=(3, 0))

        controls = tk.Frame(info, bg=PANEL)
        controls.pack(fill="x", pady=(8, 0))
        self.pause_button = ttk.Button(
            controls, text="Pause display", command=self.toggle_pause,
        )
        self.pause_button.pack(side="left")
        self.focus_button = ttk.Button(
            controls, text="Hide focus box", command=self.toggle_focus,
        )
        self.focus_button.pack(side="left", padx=(6, 0))
        ttk.Button(controls, text="Refresh now", command=self.refresh).pack(
            side="left", padx=(6, 0),
        )
        tk.Label(
            controls, text="Display only • no camera • no VLC control",
            bg=PANEL, fg=MUTED, font=("DejaVu Sans", 8),
        ).pack(side="right")

        root.bind("<space>", lambda _event: self.toggle_pause())
        root.bind("f", lambda _event: self.toggle_focus())
        root.bind("<Escape>", lambda _event: root.iconify())
        self.canvas.bind("<Configure>", lambda _event: self.fit_image())
        root.after(100, self.tick)
        self.refresh()

    def status(self) -> dict[str, object]:
        try:
            rows = STATUS_PATH.read_text(encoding="utf-8", errors="replace").splitlines()
            rows += [""] * max(0, 6 - len(rows))
            return {
                "active": rows[0].strip() == "1",
                "source": rows[1].strip() or "Unknown",
                "width": int(rows[2] or 0),
                "height": int(rows[3] or 0),
                "frames": int(rows[4] or 0),
                "timestamp": int(rows[5] or 0),
            }
        except (OSError, ValueError):
            return {
                "active": False, "source": "No source",
                "width": 0, "height": 0, "frames": 0, "timestamp": 0,
            }

    def focus(self) -> dict[str, object] | None:
        try:
            value = json.loads(FOCUS_PATH.read_text(encoding="utf-8"))
            coords = [float(value[key]) for key in ("x", "y", "width", "height")]
            if not all(math.isfinite(n) and 0 <= n <= 1 for n in coords):
                return None
            if coords[2] <= 0 or coords[3] <= 0:
                return None
            if coords[0] + coords[2] > 1.001 or coords[1] + coords[3] > 1.001:
                return None
            label = str(value.get("label", "Selected region")).replace("\n", " ").strip()
            return {
                "x": coords[0], "y": coords[1], "width": coords[2],
                "height": coords[3], "label": label[:128] or "Selected region",
            }
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def toggle_pause(self) -> None:
        self.paused = not self.paused
        self.pause_button.configure(
            text="Resume display" if self.paused else "Pause display",
        )
        if not self.paused:
            self.refresh()

    def toggle_focus(self) -> None:
        self.show_focus = not self.show_focus
        self.focus_button.configure(
            text="Hide focus box" if self.show_focus else "Show focus box",
        )
        self.draw_focus()

    def refresh(self) -> None:
        if not self.paused:
            try:
                stamp = FRAME_PATH.stat().st_mtime_ns
                if stamp != self.last_frame_mtime:
                    self.raw_photo = tk.PhotoImage(file=str(FRAME_PATH))
                    self.last_frame_mtime = stamp
                    self.fit_image()
            except (OSError, tk.TclError):
                pass
        self.draw_focus()

    def fit_image(self) -> None:
        if not self.raw_photo:
            return
        canvas_width = max(1, self.canvas.winfo_width())
        canvas_height = max(1, self.canvas.winfo_height())
        raw_width, raw_height = self.raw_photo.width(), self.raw_photo.height()
        factor = max(1, math.ceil(max(raw_width / canvas_width, raw_height / canvas_height)))
        self.photo = self.raw_photo.subsample(factor, factor) if factor > 1 else self.raw_photo
        self.image_width, self.image_height = self.photo.width(), self.photo.height()
        self.canvas.itemconfigure(self.image_item, image=self.photo)
        self.canvas.tag_lower(self.image_item)
        self.canvas.coords(self.image_item, 0, 0)
        self.draw_focus()

    def draw_focus(self) -> None:
        status = self.status()
        focus = self.focus()
        active = bool(status["active"])
        self.live_label.configure(
            text="EYES ACTIVE" if active else "WAITING FOR EYES",
            fg=FOCUS_COLOR if active else WAITING_COLOR,
        )
        self.source_label.configure(text=f"Source: {status['source']}")
        self.details_label.configure(
            text=(
                f"Native input: {status['width']}×{status['height']}  •  "
                f"Frame: {status['frames']}  •  Display: {self.image_width}×{self.image_height}  •  "
                f"Focus: {focus['label'] if focus else 'No focus region reported yet'}"
            ) if self.image_width else "Waiting for a frame from R2's existing Eyes subsystem."
        )
        if not active or not self.show_focus or not focus or not self.image_width or not self.image_height:
            self.canvas.itemconfigure(self.focus_box, state="hidden")
            self.canvas.itemconfigure(self.focus_label, state="hidden")
            return

        x1 = float(focus["x"]) * self.image_width
        y1 = float(focus["y"]) * self.image_height
        x2 = (float(focus["x"]) + float(focus["width"])) * self.image_width
        y2 = (float(focus["y"]) + float(focus["height"])) * self.image_height
        self.canvas.coords(self.focus_box, x1, y1, x2, y2)
        self.canvas.itemconfigure(self.focus_box, state="normal")
        self.canvas.coords(self.focus_label, max(4, x1), max(4, y1 - 18))
        self.canvas.itemconfigure(self.focus_label, text=str(focus["label"]), state="normal")

    def tick(self) -> None:
        self.refresh()
        self.root.after(100, self.tick)


def main() -> None:
    if not os.environ.get("DISPLAY"):
        return
    DATA_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
    with LOCK_PATH.open("a+", encoding="utf-8") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        root = tk.Tk()
        VWebcam(root)
        root.mainloop()


if __name__ == "__main__":
    main()
