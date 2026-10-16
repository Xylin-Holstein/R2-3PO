#!/usr/bin/env python3
"""Missing sensory objects must be reported as unavailable, not merely inactive."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

for function, subsystem in (
    ("int r2_eyes_status(void)", "eyes"),
    ("int r2_ears_status(void)", "ears"),
):
    start = core.index(function)
    end = core.index("\n}", start) + 2
    body = core[start:end]
    assert f"int state = {subsystem} ?" in body, f"{subsystem} status must inspect subsystem availability"
    assert ": -1;" in body, f"missing {subsystem} object must return unavailable (-1)"
    assert "pthread_mutex_lock" in body and "pthread_mutex_unlock" in body, f"{subsystem} status reads must synchronize with device lifecycle"

# The shell's existing tri-state rendering must preserve the distinction:
# >0 = active, 0 = inactive, <0 = unavailable.
for function in ("static void shell_eyes", "static void shell_ears"):
    start = shell.index(function)
    # Limit to this handler, rather than accepting unrelated state formatting.
    end = shell.find("\nstatic void ", start + len(function))
    body = shell[start:end if end >= 0 else len(shell)]
    assert 'state > 0' in body
    assert 'state == 0' in body
    assert '"UNAVAILABLE"' in body

print("Sensory status smoke check passed: absent Eyes/Ears are unavailable, not inactive.")
