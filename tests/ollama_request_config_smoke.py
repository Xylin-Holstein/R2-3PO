#!/usr/bin/env python3
"""Guard against silently omitting Ollama request metadata/options."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


start = source.find("static char *ollama_chat_with_limit(")
end = source.find("static char *", start + 1)
if start < 0 or end <= start:
    raise SystemExit("FAIL: could not isolate ollama_chat_with_limit()")
function = source[start:end]

require("ollama_json_object_add_checked(root, \"model\"," in function
        and "ollama_json_object_add_checked(root, \"stream\"," in function
        and "ollama_json_object_add_checked(root, \"think\"," in function
        and "ollama_json_object_add_checked(root, \"keep_alive\"," in function,
        "required top-level model settings are checked")
require("struct json_object *options = json_object_new_object();" in function
        and "if (!options)" in function
        and "R2_OLLAMA_ADD_OPTION(\"num_ctx\"" in function
        and "R2_OLLAMA_ADD_OPTION(\"num_predict\"" in function
        and "R2_OLLAMA_ADD_OPTION(\"temperature\"" in function
        and "R2_OLLAMA_ADD_OPTION(\"top_p\"" in function
        and "R2_OLLAMA_ADD_OPTION(\"num_gpu\"" in function,
        "required generation options and optional GPU placement are checked")
require("ollama_json_object_add_checked(root, \"options\", options) != 0" in function,
        "the complete options object must attach successfully")
require("ollama_json_append_message(arr, \"system\", system_override," in function,
        "the system override uses checked message serialization")
require("json_object_object_add(root, \"model\", json_object_new_string(MODEL))" not in function
        and "if (options) {" not in function,
        "old unchecked metadata/options paths are absent")

progress_start = source.find("static int ollama_progress(")
progress_end = source.find("\nint r2_ollama_vision_request_begin", progress_start)
if progress_start < 0 or progress_end <= progress_start:
    raise SystemExit("FAIL: could not isolate foreground inference progress callback")
progress = source[progress_start:progress_end]
require("CLOCK_MONOTONIC" in progress
        and "Still waiting for local Ollama response; stage=%s" in progress
        and "since_notice >= 15" in progress
        and "!ollama_background_mode" in progress
        and "ollama_progress_clock_valid = 0;" in function,
        "long foreground inference reports bounded progress while background requests remain quiet")

response_start = source.find("struct json_object *resp = json_tokener_parse(b.data);")
response_end = source.find("static char *ollama_chat(", response_start)
if response_start < 0 or response_end <= response_start:
    raise SystemExit("FAIL: could not isolate Ollama response contract validation")
response = source[response_start:response_end]
require('json_object_is_type(role_obj, json_type_string)' in response
        and 'json_object_is_type(done_obj, json_type_boolean)' in response,
        "response role and completion flag must have their documented JSON types")
require("ollama_response_contract_valid(response_role, response_done)" in response
        and 'strcmp(role, "assistant") == 0 && done == 1' in source
        and 'ollama_invalid_completion_contract' in response,
        "only a complete assistant response is accepted as the final answer")

print("Ollama request configuration smoke test passed.")
