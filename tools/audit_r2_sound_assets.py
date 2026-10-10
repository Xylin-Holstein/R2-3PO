#!/usr/bin/env python3
"""Read-only inventory of R2's named sound assets; never opens audio payloads."""
from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

STATE_TOKENS = {
    "curious", "curiosity", "happy", "happiness", "joy", "cheerful",
    "pleased", "confused", "confusion", "uncertain", "puzzled", "alert",
    "attention", "warning", "alarm", "sad", "sadness", "disappointed",
    "sleepy", "tired", "exhausted", "drowsy", "hungry", "hunger",
    "excited", "excitement", "enthusiastic", "thinking", "think",
    "processing", "pondering", "greeting", "greet", "hello", "welcome",
    "acknowledge", "acknowledgement", "confirm", "affirmative", "yes",
    "neutral", "default", "idle", "normal", "generic", "calm", "playful",
    "worried", "surprised", "content", "frustrated", "sleep", "resting",
}
GENERIC = {"default", "generic", "normal", "idle", "neutral"}
BEEP = {"beep", "beeps", "boop", "boops", "bleep", "bleeps"}
WHISTLE = {"whistle", "whistles", "whistling"}
STATES_TO_REPORT = ("curious", "happy", "confused", "alert", "sleepy",
                    "hungry", "excited", "thinking", "greeting")


def tokens(filename: str) -> set[str]:
    word: list[str] = []
    result: set[str] = set()
    for char in filename.lower():
        if char.isalnum():
            word.append(char)
        elif word:
            result.add("".join(word))
            word.clear()
    if word:
        result.add("".join(word))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="R2 FX directory (inspected read-only)")
    args = parser.parse_args()
    root = args.root
    if not root.is_dir() or not os.access(root, os.R_OK | os.X_OK):
        print(f"ERROR: sound directory is missing or unreadable: {root}")
        return 2
    try:
        files = sorted((p for p in root.iterdir()
                        if p.suffix.lower() == ".mp3" and p.is_file()),
                       key=lambda p: p.name.casefold())
    except OSError as exc:
        print(f"ERROR: cannot list sound directory {root}: {exc}")
        return 2

    print(f"R2 sound asset inventory: {root}")
    print(f"Readable regular MP3 files: {len(files)}")
    by_kind: dict[str, list[tuple[Path, set[str]]]] = {"beep": [], "whistle": []}
    for path in files:
        words = tokens(path.name)
        if words & BEEP:
            by_kind["beep"].append((path, words))
        if words & WHISTLE:
            by_kind["whistle"].append((path, words))
    for kind, entries in by_kind.items():
        print(f"{kind.title()} candidates: {len(entries)}")
        for path, words in entries:
            states = sorted(words & STATE_TOKENS)
            labels = ", ".join(states) if states else (
                "generic" if words & GENERIC else "unlabelled fallback")
            print(f"  {path.name} [{labels}]")
        if not entries:
            print(f"  WARNING: no filename-token match for {kind}")

    for state in STATES_TO_REPORT:
        kinds = []
        for kind, entries in by_kind.items():
            if any(state in words or words & GENERIC for _, words in entries):
                kinds.append(kind)
        print(f"State {state}: {', '.join(kinds) if kinds else 'no named match'}")

    players = [name for name in ("mpg123", "ffplay", "mpv") if shutil.which(name)]
    print("Available players: " + (", ".join(players) if players else "none found"))
    print("Note: inventory does not decode MP3s or confirm speaker output.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
