#!/usr/bin/env python3
"""Regression guard for multi-turn conversation recovery and turn serialization."""
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
    start = core.find(start_marker)
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

# Persist the actual current user turn before model work, so a failed inference
# can be retried without silently dropping or rewriting the user's words.
add_user = impl.find('message_add("user", message)')
first_generation = impl.find("chat_with_relevant_memories(message)")
require(
    add_user >= 0 and first_generation > add_user,
    "the original user turn is stored before contextual generation begins",
)
require(
    core.includes("RELATIONSHIP AND NAME CONTINUITY")
    and "quoted message, an archived conversation, a diary" in core
    and "prioritize the correction" in core,
    "explicit family relationships survive name contamination from quoted/retrieved history while current corrections win",
)

require(
    impl.count("chat_with_relevant_memories(message)") >= 3
    and "first contextual response attempt failed" in impl
    and "returning a visible fallback and preserving the user turn" in impl,
    "generation failure has a refreshed retry plus a visible final fallback",
)

# A model response containing only internal markers must not trigger an action.
visible_gate = impl.find("if (!reply_has_visible_text(stripped_reply))")
tool_execution = impl.find("char *tools = process_tools(stripped_reply)")
require(
    visible_gate >= 0 and tool_execution > visible_gate
    and "Retry still produced no visible reply" in impl,
    "marker-only/empty model output is retried or replaced before tool execution",
)
require(
    "if (!tools)" in impl
    and "couldn't finish processing that response" in impl
    and "goto finalize_reply;" in impl,
    "tool-processing failure is converted into a visible response instead of a silent turn",
)

# The public entry point must serialize entire turns, while announcing intent
# before waiting so autonomous background work can yield to queued user input.
lock = public.find("pthread_mutex_lock(&conversation_turn_lock)")
intent = public.find("atomic_fetch_add(&foreground_conversation_waiting, 1)")
call = public.find("r2_talk_impl(message)")
unlock = public.find("pthread_mutex_unlock(&conversation_turn_lock)")
decrement = public.find("atomic_fetch_sub(&foreground_conversation_waiting, 1)")
require(
    intent >= 0 and lock > intent and call > lock and unlock > call
    and decrement > unlock,
    "foreground intent is announced before lock wait and turn ownership spans the full implementation",
)
require(
    "reply = xstrdup(" in public
    and "returning an explicit failure message" in public
    and public.find("reply = xstrdup(") > call,
    "the public API has its own non-empty fallback if the implementation fails",
)

# Background needs work must never wait behind or mutate a live user turn.
food = function_slice(
    "static void autonomous_needs_food_safeguard(void)",
    "/* Keep the autonomous controller on the smallest safe action allowlist. */",
)
require(
    "pthread_mutex_trylock(&conversation_turn_lock)" in food
    and "atomic_load(&foreground_ollama_waiting) > 0" in food
    and "atomic_load(&foreground_conversation_waiting) > 0" in food,
    "autonomous needs safeguard yields to foreground model and conversation work",
)
require(
    "pthread_mutex_trylock(&conversation_turn_lock)" in core
    and "autonomous_needs_stale_action" in core,
    "autonomous action path uses a non-blocking turn guard and stale-action protection",
)

# Keep action side effects and their history representation paired; never
# preserve raw model control syntax as a future instruction.
require(
    'message_add_pair_locked(\n                    "assistant", tool_request_context, "user", tagged_tool_result)'
    in impl
    and "[R2_INTERNAL_TOOL_RESULT]" in impl
    and "raw [WORLD]/[RUN]/etc. commands into later model context" in impl,
    "executed tool actions are recorded as an atomic tagged result, not replayable raw commands",
)

print("PASS: conversation recovery and serialization regression suite")
