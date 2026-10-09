#!/usr/bin/env python3
"""Smoke-test observer privacy filtering without a desktop."""
import importlib.util
from pathlib import Path
path = Path(__file__).resolve().parents[1] / "Observer.py"
spec = importlib.util.spec_from_file_location("r2_observer", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.is_observer_event("world", "object_moved")
assert module.is_observer_event("media", "activity_ended")
assert module.is_observer_event("world", "location_changed")
assert not module.is_observer_event("conversation", "conversation_turn")
assert not module.is_observer_event("thinking", "thought")
assert not module.is_observer_event("world", "internal_notification")
assert not module.is_observer_event("world", "unknown_new_event")
print("Observer allowlist smoke test passed.")
