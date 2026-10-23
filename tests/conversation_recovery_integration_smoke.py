#!/usr/bin/env python3
"""Regression guard for R2's real multi-turn recovery and lifecycle path."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


def function_slice(start_marker: str, end_marker: str) -> str:
    start = core.rfind(start_marker)
    end = core.find(end_marker, start + 1) if start >= 0 else -1
    if start < 0 or end < 0:
        raise SystemExit(f"Could not isolate {start_marker!r}")
    return core[start:end]


impl = function_slice(
    "static char *r2_talk_impl(const char *message)",
    "char *r2_talk(const char *message)",
)
public = function_slice(
    "char *r2_talk(const char *message)",
    "int r2_conversation(void)",
)
add_user = impl.find('message_add("user", message)')
first_generation = impl.find("chat_with_relevant_memories(message)")
require(add_user >= 0 and first_generation > add_user,
        "the original user turn is stored before contextual generation begins")

misread_start = core.find("static int r2_reply_misclassifies_plain_text(")
misread_end = core.find("\nstatic char *r2_retry_direct_answer(", misread_start)
require(misread_start >= 0 and misread_end > misread_start,
        "the nonresponsive-reply classifier exists")
misread = core[misread_start:misread_end]
require(
    "r2_reply_contains_generic_clarification_failure(reply)" in misread
    and "r2_reply_contains_generic_clarification_failure(" in core
    and "user_clarification_terms" in misread
    and '"can you explain"' in misread
    and '"could you explain"' in misread
    and '"what do you mean"' in misread,
    "substantive generic clarification failures trigger recovery while genuine explanation requests do not",
)
require(
    'r2_reply_misclassifies_plain_text(query, reply ? reply : "")' in core
    and "chat_with_relevant_memories(message)" in impl,
    "the production conversation path invokes the helper that runs the recovery classifier",
)

retry_builder = function_slice(
    "static char *r2_build_retry_system_prompt(void)",
    "static int r2_is_retry_context_message(",
)
require(
    "memcpy(write, SYSTEM_PROMPT, system_length);" in retry_builder
    and "r2_conversation_system_prompt, conversation_length" in retry_builder
    and "memcpy(write, retry_instruction, retry_length);" in retry_builder
    and "identity_end_marker" not in retry_builder,
    "corrective retry preserves the full static prompt and shared conversation policy",
)
require(
    "RELATIONSHIP AND NAME CONTINUITY" not in core
    and "If explicit persistent memory establishes that the current user is your mother" not in core
    and "AFFECT AND NONVERBAL EXPRESSION:" not in core,
    "relationships and affect are learned from interaction rather than prescribed as a role",
)

serializer = function_slice(
    "static char *ollama_chat_with_limit(",
    "\nstatic char *ollama_chat(",
)
require(
    "r2_is_static_system_prompt_section(&msgs[i])" in serializer
    and "if (required_chars > input_budget)" in serializer
    and "include[current_user_index] = 1;" in serializer,
    "the request allocator preserves the complete static instructions and active user turn",
)
require(
    '"ollama_current_user_turn_missing"' in serializer
    and "complete instructions and the active user turn" in serializer
    and "authoritative current user turn is missing" in serializer,
    "missing authoritative turns and over-budget complete prompts are reported rather than silently replaced",
)

visible_start = core.find("static int reply_has_visible_text(")
visible_end = core.find("\nstatic ", visible_start + 1)
visible_helper = core[visible_start:visible_end + 3] if visible_start >= 0 and visible_end > visible_start else ""
require(
    "control_blocks[]" in visible_helper
    and '"[RUN]"' in visible_helper
    and '"[WORLD]"' in visible_helper
    and "Malformed control output is never user-facing text" in visible_helper,
    "internal action markers do not count as a visible user-facing answer",
)
visible_gate = impl.find("if (!reply_has_visible_text(stripped_reply))")
tool_execution = impl.find("char *tools = process_tools(stripped_reply)")
require(visible_gate >= 0 and tool_execution > visible_gate,
        "empty or marker-only replies are handled before action execution")

intent = public.find("atomic_fetch_add(&foreground_conversation_waiting, 1)")
lock = public.find("pthread_mutex_lock(&conversation_turn_lock)")
call = public.find("r2_talk_impl(message)")
unlock = public.find("pthread_mutex_unlock(&conversation_turn_lock)")
decrement = public.find("atomic_fetch_sub(&foreground_conversation_waiting, 1)")
require(
    intent >= 0 and lock > intent and call > lock and unlock > call and decrement > unlock,
    "foreground intent is announced before lock wait and the turn lock spans the complete implementation",
)

needs = function_slice("static void autonomous_needs_check(void)", "int r2_think(void)")
require(
    "pthread_mutex_trylock(&conversation_turn_lock)" in needs
    and "atomic_load(&foreground_ollama_waiting) > 0" in needs
    and "atomic_load(&foreground_conversation_waiting) > 0" in needs
    and "autonomous_needs_stale_action" in needs,
    "autonomous needs work yields to foreground conversation and rejects stale actions",
)

request_shutdown = function_slice("void r2_request_shutdown(void)", "int r2_request_restart(void)")
for label, body in (
    ("public shutdown", request_shutdown),
    ("core shutdown", core[core.rfind("void r2_shutdown(void)"): ]),
):
    lock_at = body.find("pthread_mutex_lock(&task_queue_lock)")
    state_at = body.find("shutting_down = 1;")
    wake_at = body.find("pthread_cond_broadcast(&task_queue_cond)")
    unlock_at = body.find("pthread_mutex_unlock(&task_queue_lock)")
    require(lock_at >= 0 and state_at > lock_at and wake_at > state_at and unlock_at > wake_at,
            f"{label} changes the worker predicate and broadcasts under the queue mutex")

shutdown_tail = core[core.find("void r2_shutdown(void)"):]
hands_join = shutdown_tail.find("pthread_join(hands_thread, NULL)")
pending_drain = shutdown_tail.find("Task *pending_tasks = task_head")
require(
    hands_join >= 0 and pending_drain > hands_join
    and "task_head = NULL;" in shutdown_tail[pending_drain:]
    and "task_tail = NULL;" in shutdown_tail[pending_drain:]
    and "free(pending_tasks->command);" in shutdown_tail[pending_drain:],
    "shutdown reclaims queued task nodes after the Hands worker stops",
)
require(
    'message_add_pair_locked(\n                    "assistant", tool_request_context, "user", tagged_tool_result)' in impl
    and "[R2_INTERNAL_TOOL_RESULT]" in impl,
    "completed actions remain paired with a tagged internal result rather than replayable raw commands",
)
require(
    "r2_log_recent_conversation_context(3000U)" in core
    and "RECENT PERSISTED CONVERSATION CONTEXT" in core,
    "durable recent conversation is restored at startup",
)
print("Conversation recovery and turn serialization regression suite passed.")
