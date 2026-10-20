#!/usr/bin/env python3
"""Static regression checks for R2's Imagination and Sensory Journal wiring."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
log = (ROOT / "Log.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
reward = (ROOT / "Reward.c").read_text(encoding="utf-8")
addiction = (ROOT / "Addiction.c").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")

for path in ("Imagination.c", "Imagination.h", "SensoryJournal.c", "SensoryJournal.h"):
    assert (ROOT / path).is_file(), f"missing subsystem file: {path}"

assert '#include "Imagination.h"' in core
assert "r2_imagination_init()" in core and "r2_imagination_shutdown();" in core

# A failed persistent-history query must not be represented as zero points or
# an empty repeated-interest history.
reward_start = reward.find("char *r2_reward_context(void)")
reward_end = reward.find("\nint r2_reward_review_diary(", reward_start)
if reward_start < 0 or reward_end < 0:
    raise SystemExit("Could not locate reward context report.")
reward_context = reward[reward_start:reward_end]
for marker in (
    "if (rc != SQLITE_ROW)",
    "if (!omitted && !failed && rc != SQLITE_DONE)",
    "Reward context unavailable: the cumulative reward total could not be read.",
    "Reward context unavailable: recent reward events could not be read completely.",
):
    if marker not in reward_context:
        raise SystemExit(f"Reward context masks an incomplete read: {marker!r}")

addiction_start = addiction.find("char *r2_addiction_report(void)")
if addiction_start < 0:
    raise SystemExit("Could not locate Addiction context report.")
addiction_context = addiction[addiction_start:]
for marker in (
    "if (rc != SQLITE_OK)",
    "if (!omitted && !failed && rc != SQLITE_DONE)",
    "Addiction context unavailable: learning records could not be read completely.",
):
    if marker not in addiction_context:
        raise SystemExit(f"Addiction context masks an incomplete read: {marker!r}")

reality_init_start = reality.find("int r2_reality_init(void)")
reality_init_end = reality.find("\nvoid r2_reality_shutdown(void)", reality_init_start)
if reality_init_start < 0 or reality_init_end < 0:
    raise SystemExit("Could not locate Reality-owned Addiction lifecycle.")
reality_init = reality[reality_init_start:reality_init_end]
if "if (r2_addiction_init() != 0)" not in reality_init or "r2_reality_shutdown();" not in reality_init:
    raise SystemExit("Reality must propagate Addiction initialization failure instead of continuing without preference history.")
reality_shutdown_start = reality.find("void r2_reality_shutdown(void)")
reality_shutdown_end = reality.find("\nint r2_reality_is_initialized(void)", reality_shutdown_start)
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

# TV status must not trust malformed persistent rows or let snprintf's required
# length advance the buffer cursor beyond the allocation.
tv_start = reality.find("static int append_tv_status(")
tv_end = reality.find("\nint r2_reality_tv_display_event(", tv_start)
if tv_start < 0 or tv_end < 0:
    raise SystemExit("Could not locate bounded TV status builder.")
tv_status = reality[tv_start:tv_end]
for marker in (
    "vsnprintf(out + *length, capacity - *length, format, args)",
    "if (written < 0 || (size_t)written >= capacity - *length) return -1;",
    "if (sqlite3_column_type(st, 0) != SQLITE_INTEGER",
    "SQLITE_CORRUPT",
    "tv_status_error:",
    "pthread_mutex_unlock(&reality_lock);",
):
    if marker not in tv_status:
        raise SystemExit(f"TV status must validate data and append bounds: {marker!r}")

reality_init_start = reality.find("int r2_reality_init(void)")
reality_init_end = reality.find("\nvoid r2_reality_shutdown(void)", reality_init_start)
reality_init = reality[reality_init_start:reality_init_end]
for marker in (
    "Could not read a valid persistent location; refusing to assume home",
    "Could not initialize required Addiction/preference history.",
    "Could not refresh authoritative wallet state after initialization.",
):
    if marker not in reality_init:
        raise SystemExit(f"Reality startup must propagate authoritative state failure: {marker!r}")

# Ensure this integration retains current-turn identity and the reply-recovery path.
assert "conversation_turn_lock" in core
assert "r2_reply_reverses_speaker_roles" in core
assert 'if (strcmp(m->role, "user") == 0 &&\n            r2_recent_message_is_tool_payload(m->content))' not in core

print("Imagination, Sensory Journal, Reality, and conversation wiring smoke passed.")
