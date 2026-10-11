#!/usr/bin/env python3
"""Source-contract regression for autonomous needs safety and single-meal cycles."""

from pathlib import Path

source = Path("r2.c").read_text(encoding="utf-8")
start = source.index("static void autonomous_needs_check(void)\n{")
end = source.index("\nint r2_think(void)", start)
function = source[start:end]

allow_start = source.index("static int autonomous_needs_action_allowed(")
allow_end = source.index("\n/* Re-check the authoritative need", allow_start)
allow_guard = source[allow_start:allow_end]
assert 'command_len == 3 && !strncmp(command, "eat", 3)' in allow_guard
assert 'command_len == 10 && !strncmp(command, "fridge_eat", 10)' in allow_guard
assert 'command_len == 5 && !strncmp(command, "sleep", 5)' in allow_guard
assert "autonomous_needs_action_allowed(safe_action)" in function
assert "autonomous_needs_action_rejected" in function

helper_start = source.index("static int autonomous_needs_action_still_needed(")
helper_end = source.index("\n/* Autonomous needs checks are deliberately limited", helper_start)
food_guard = source[helper_start:helper_end]
assert 'strstr(action, "[WORLD] eat|")' in food_guard
assert 'strstr(action, "[WORLD] fridge_eat|")' in food_guard
assert 'double value = strtod(field, &end);' in food_guard
assert "value >= 50.0" in food_guard
assert "value >= 85.0" in food_guard
assert 'strstr(action, "[WORLD] sleep|")' in food_guard
assert 'const char *label = is_food_action ? "Hunger: " : "Sleepiness: ";' in food_guard
assert "if (!status) return 0;" in food_guard
assert "if (!autonomous_needs_action_still_needed(safe_action))" in function
assert "autonomous_needs_stale_action" in function

assert "int food_action_succeeded = 0;" in function
assert "int world_action_succeeded = 0;" in function
assert 'strstr(result, "REAL NEEDS RESULT: eating \'") != NULL' in function
assert 'strstr(result, "updated persistent hunger and energy.") != NULL' in function
assert 'strstr(result, "FRIDGE RESULT: eating \'") != NULL' in function
assert 'strstr(result, "reduced hunger and consumed fridge stock.") != NULL' in function
assert "REAL NEEDS RESULT: sleep transition was persisted." in function
assert "if (!world_action_succeeded)\n        autonomous_needs_food_safeguard();" in function
assert "autonomous_needs_yielded_to_conversation" in function
assert "pthread_mutex_trylock(&conversation_turn_lock)" in function

talk_start = source.index("char *r2_talk(const char *message)\n{")
talk_end = source.index("\nint r2_conversation(void)", talk_start)
talk_wrapper = source[talk_start:talk_end]
assert talk_wrapper.index("atomic_fetch_add(&foreground_conversation_waiting, 1)") < talk_wrapper.index("pthread_mutex_lock(&conversation_turn_lock)")
assert "atomic_fetch_sub(&foreground_conversation_waiting, 1)" in talk_wrapper
assert "ollama_foreground_turn_mode = 1;" in talk_wrapper
assert "ollama_foreground_turn_mode = previous_turn_mode;" in talk_wrapper
assert "(atomic_load(&foreground_conversation_waiting) > 0 &&" in source
assert "!ollama_foreground_turn_mode" in source
vision_begin_start = source.index("int r2_ollama_vision_request_begin(void)")
vision_begin_end = source.index("\nvoid r2_ollama_vision_request_end(void)", vision_begin_start)
vision_begin = source[vision_begin_start:vision_begin_end]
assert "foreground_conversation_waiting" in vision_begin
assert "!ollama_foreground_turn_mode" in vision_begin
vision_abort_start = source.index("int r2_ollama_vision_request_should_abort(void)")
vision_abort_end = source.index("\n/* Estimate safe Ollama context", vision_abort_start)
assert "!ollama_foreground_turn_mode" in source[vision_abort_start:vision_abort_end]

worker_start = source.index("static void *post_turn_worker(void *opaque)\n{")
worker_end = source.index("\n/* These model-assisted bookkeeping tasks", worker_start)
worker = source[worker_start:worker_end]
assert "while (!shutting_down" in worker
assert "atomic_load(&foreground_conversation_waiting) > 0" in worker
assert worker.index("atomic_load(&foreground_conversation_waiting) > 0") < worker.index("log_structured_self_report")

safeguard_start = source.index("static void autonomous_needs_food_safeguard(void)\n{")
assert "pthread_mutex_trylock(&conversation_turn_lock) != 0" in source[safeguard_start:start]

# A successful meal or sleep satisfies this cycle's action; the fallback only
# runs when no successful allowed action occurred.
assert function.count("autonomous_needs_food_safeguard();") == 4, (
    "expected fallbacks for missing state, allocation failure, model failure, "
    "and the final guarded fallback"
)
print("autonomous needs safety smoke passed")
