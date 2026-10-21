#!/usr/bin/env python3
"""Source contract for fail-closed Game Boy command JSON handling."""
from pathlib import Path

source = Path("r2.c").read_text(encoding="utf-8")

helper_start = source.index("static int gameboy_json_bool(")
helper_end = source.index("\n}", helper_start) + 2
helper = source[helper_start:helper_end]
assert "json_type_boolean" in helper, "JSON flags must be actual booleans"
assert "return fallback;" in helper, "missing or wrongly typed flags must use fail-closed fallback"

dispatch_start = source.index("int response_is_list =")
dispatch_end = source.index(";", source.index("int command_ok =", dispatch_start)) + 1
dispatch = source[dispatch_start:dispatch_end]
assert 'strcmp(command, "list")' in dispatch and "json_type_array" in dispatch, (
    "the list command must accept its documented JSON-array response"
)
assert 'strcmp(command, "status")' in dispatch and "json_type_object" in dispatch, (
    "status must require its documented JSON-object response"
)
assert 'gameboy_json_bool(console_json, "ok", 0)' in dispatch, (
    "action commands must require an explicit JSON boolean true"
)
assert "command_rc == 0" in dispatch and "response_is_success_object" in dispatch, (
    "a zero process exit alone must not imply a successful command"
)

print("Game Boy command success requires a valid documented JSON response.")
