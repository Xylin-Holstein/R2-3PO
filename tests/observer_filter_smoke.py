#!/usr/bin/env python3
"""Smoke-test observer privacy filtering without a desktop."""
import importlib.util
from pathlib import Path
path = Path(__file__).resolve().parents[1] / "Observer.py"
spec = importlib.util.spec_from_file_location("r2_observer", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.is_observer_event("world", "object_moved")
assert module.is_observer_event("world", "creator_gift")
assert module.is_observer_event("world", "enjoyment_changed")
assert module.is_observer_event("world", "addiction_status_changed")
assert module.is_observer_event("media", "activity_ended")
assert module.is_observer_event("world", "location_changed")
assert module.is_observer_event("world", "gameboy_device_action")
assert module.is_observer_event("world", "gameboy_game_started")
assert module.is_observer_event("world", "gameboy_game_event")
assert not module.is_observer_event("world", "controller_input")
assert not module.is_observer_event("conversation", "conversation_turn")
assert not module.is_observer_event("thinking", "thought")
assert not module.is_observer_event("world", "internal_notification")
assert not module.is_observer_event("world", "unknown_new_event")
print("Observer allowlist smoke test passed.")

from pathlib import Path
import tempfile

with tempfile.TemporaryDirectory() as directory:
    output = Path(directory) / "R2_Diary" / "Activity.txt"
    module.append_activity_line(output, "2026-10-09 09:15:00", "R2 moved to kitchen.", 17)
    module.append_activity_line(output, "2026-10-09 09:16:00", "R2 picked up a book.", 18)
    contents = output.read_text(encoding="utf-8")
    assert contents.count("R2-3PO Activity Log") == 1
    assert "[2026-10-09 09:15:00] R2 moved to kitchen. (event #17)" in contents
    assert "[2026-10-09 09:16:00] R2 picked up a book. (event #18)" in contents
    assert contents.index("R2 moved to kitchen") < contents.index("R2 picked up a book")
print("Activity.txt append smoke test passed.")
