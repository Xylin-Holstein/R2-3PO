#!/usr/bin/env python3
"""Guard against silently omitting Ollama messages on allocation failure."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


helper_start = source.find("static int ollama_json_append_message(")
helper_end = source.find("static char *ollama_chat_with_limit(", helper_start)
if helper_start < 0 or helper_end <= helper_start:
    raise SystemExit("FAIL: could not isolate ollama_json_append_message()")
helper = source[helper_start:helper_end]

require("json_object_new_object()" in helper
        and "json_object_new_string(role)" in helper
        and "json_object_new_string_len(content, (int)content_length)" in helper,
        "message, role, and content allocations are all checked")
require("json_object_object_add(message, \"role\", role_value) != 0" in helper
        and "json_object_object_add(message, \"content\", content_value) != 0" in helper
        and "json_object_array_add(array, message) != 0" in helper,
        "JSON insertion failures are treated as serialization failures")
require("if (role_value) json_object_put(role_value);" in helper
        and "if (content_value) json_object_put(content_value);" in helper
        and "if (message) json_object_put(message);" in helper,
        "partial JSON objects are cleaned up on failure")

call = source.find("if (ollama_json_append_message(arr, role, content, limits[i]) != 0)")
if call < 0:
    raise SystemExit("FAIL: serializer loop does not use checked message helper")
loop_end = source.find("\n        }\n    }\n    free(message_order);", call)
if loop_end < 0:
    raise SystemExit("FAIL: could not isolate the complete message serialization loop")
loop = source[call:loop_end]
require("return NULL;" in loop
        and "json_object_put(arr);" in loop
        and "json_object_put(root);" in loop
        and "continue;" not in loop,
        "a failed message aborts the request instead of silently skipping a turn")
require("if (json_object_object_add(root, \"messages\", arr) != 0)" in source,
        "attaching the complete message array is checked")
require("size_t override_length = system_override ? strlen(system_override) : 0;" in source,
        "the complete conversation system prompt is measured without truncation")
require("if (override_length > override_budget)" not in source
        and "bounded_override" not in source,
        "no override clipping path can silently shorten R2's instructions")
require("if (length > OLLAMA_MAX_SYSTEM_MESSAGE_CHARS)" not in source
        and "OLLAMA_MAX_SYSTEM_TOTAL_CHARS" not in source
        and "OLLAMA_MAX_TOTAL_CHARS" not in source,
        "no fixed application-level system or total-payload cap remains")

print("Ollama payload allocation smoke test passed.")
