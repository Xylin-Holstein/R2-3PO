#!/usr/bin/env python3
"""Static regression checks for R2's Imagination and Sensory Journal wiring."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
log = (ROOT / "Log.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")

for path in ("Imagination.c", "Imagination.h", "SensoryJournal.c", "SensoryJournal.h"):
    assert (ROOT / path).is_file(), f"missing subsystem file: {path}"

assert '#include "Imagination.h"' in core
assert "r2_imagination_init()" in core and "r2_imagination_shutdown();" in core
reality_init_start = reality.find("int r2_reality_init(void)")
reality_init_end = reality.find("\\nvoid r2_reality_shutdown(void)", reality_init_start)
if reality_init_start < 0 or reality_init_end < 0:
    raise SystemExit("Could not locate Reality-owned Addiction lifecycle.")
reality_init = reality[reality_init_start:reality_init_end]
if "if (r2_addiction_init() != 0)" not in reality_init or "r2_reality_shutdown();" not in reality_init:
    raise SystemExit("Reality must propagate Addiction initialization failure instead of continuing without preference history.")
reality_shutdown_start = reality.find("void r2_reality_shutdown(void)")
reality_shutdown_end = reality.find("\\nint r2_reality_is_initialized(void)", reality_shutdown_start)
if reality_shutdown_start < 0 or reality_shutdown_end < 0:
    raise SystemExit("Could not locate Reality shutdown ownership.")
if "r2_addiction_shutdown();" not in reality[reality_shutdown_start:reality_shutdown_end]:
    raise SystemExit("Reality must close the Addiction database it owns.")
if "r2_addiction_init()" in core or "r2_addiction_shutdown();" in core:
    raise SystemExit("Core must not duplicate the Addiction lifecycle owned by Reality.")
assert 'extract_marker(reply, "[IMAGINE]"' in core
assert 'extract_marker(reply, "[IMAGINE_FEEDBACK]"' in core
assert "[END IMAGINE]" in core and "[END IMAGINE_FEEDBACK]" in core
assert "r2_imagination_create(trim(request))" in core
assert "r2_imagination_feedback(branch_id, assessment, notes)" in core

assert '#include "SensoryJournal.h"' in core
assert "r2_sj_init()" in core and "r2_sj_shutdown();" in core
for init_failure in (
    'R2_INIT_FAIL("could not initialize R2 Sensory Journal")',
    'R2_INIT_FAIL("could not initialize R2 reward ledger")',
    'R2_INIT_FAIL("could not initialize R2 Visual Experience Library")',
    'R2_INIT_FAIL("could not initialize R2 Alternate-Self Lab")',
    'R2_INIT_FAIL("could not initialize R2 Imagination")',
):
    assert init_failure in core, f"core startup silently degrades instead of requiring {init_failure}"
assert "r2_sj_recent((int)limit)" in core
assert "r2_sj_search(arg1, (int)limit)" in core
assert "r2_sj_note(arg1" in core
assert 'extract_marker(reply, "[SENSORY_JOURNAL]"' in core

assert '#include "SensoryJournal.h"' in log
index_call = log.find("r2_sj_index_event(inserted_id")
unlock_call = log.find("pthread_mutex_unlock(&log_lock);", log.find("int64_t r2_log_event("))
assert index_call > unlock_call, "journal indexing must happen after releasing the Life Log mutex"
for category in ('"sensory"', '"thinking"', '"conversation"'):
    assert category in log, f"missing journal category: {category}"

assert "Imagination.c SensoryJournal.c" in makefile
assert "Imagination.h SensoryJournal.h" in makefile
assert '"$R2_SOURCE/Imagination.c"' in launcher
assert '"$R2_SOURCE/SensoryJournal.c"' in launcher
assert '"$R2_SOURCE/Imagination.h"' in launcher
assert '"$R2_SOURCE/SensoryJournal.h"' in launcher
assert "SensoryJournal.c" in workflow
assert "Imagination.c" in workflow

# The newer Reality-backed [WORLD] dispatcher remains the single authority for
# inventory, fridge, money, location, and TV state; no parallel World database.
assert 'extract_marker(reply, "[WORLD]"' in core
assert "r2_reality_move_item" in core and "r2_eat_and_learn" in core

# Ensure this integration retains current-turn identity and the reply-recovery path.
assert "conversation_turn_lock" in core
assert "r2_reply_reverses_speaker_roles" in core
assert 'if (strcmp(m->role, "user") == 0 &&\n            r2_recent_message_is_tool_payload(m->content))' not in core

print("Imagination, Sensory Journal, Reality, and conversation wiring smoke passed.")
