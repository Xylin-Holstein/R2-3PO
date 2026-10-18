#!/usr/bin/env python3
"""Regression guard: recent conversation context must keep the newest turns."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


start = core.find("char *r2_recent_conversation_context(")
end = core.find("/* Append complete Life Log event records", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate recent conversation context builder.")
builder = core[start:end]

require(
    "selected[found++] = index;" in builder
    and "const Message *m = &messages.items[selected[included]];" in builder,
    "bounded context budgeting walks from the newest selected turn toward older turns",
)
require(
    "size_t included = 0;" in builder
    and "line_length > max_chars - selected_chars - separator_length" in builder
    and "++included;" in builder,
    "only complete recent message lines are budgeted before formatting",
)
require(
    "for (size_t n = included; n > 0; --n)" in builder
    and "selected[n - 1]" in builder,
    "chosen recent turns are emitted in chronological order after prioritization",
)
require(
    "if (found > 0 && included == 0)" in builder
    and '" [truncated]\\n"' in builder
    and "messages.items[selected[0]]" in builder,
    "a single oversized newest turn is explicitly clipped instead of being replaced by older history",
)
require(
    "pthread_mutex_lock(&messages_lock);" in builder
    and "pthread_mutex_unlock(&messages_lock);" in builder,
    "history selection and formatting remain protected by the message-history mutex",
)
require(
    "override_length = r2_utf8_safe_prefix_length(" in core
    and "content, OLLAMA_MAX_SYSTEM_MESSAGE_CHARS);" in core
    and core.count("content, OLLAMA_MAX_MESSAGE_CHARS);") == 2
    and not re.search(r"^\\s+length = OLLAMA_MAX_MESSAGE_CHARS;$", core, re.MULTILINE)
    and not re.search(r"^\\s+length = OLLAMA_MAX_SYSTEM_MESSAGE_CHARS;$", core, re.MULTILINE),
    "Ollama hard byte caps clip system overrides and message bodies at UTF-8 boundaries",
)
require(
    "cut = r2_utf8_safe_prefix_length(context, max_chars);" in core
    and "n = r2_utf8_safe_prefix_length(found, max_chars - used);" in core,
    "shared retrieved-evidence and associated-record caps preserve UTF-8 boundaries when no full record fits",
)
print("Recent conversation context budget smoke passed.")
