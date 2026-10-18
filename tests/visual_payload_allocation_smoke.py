#!/usr/bin/env python3
"""Require transactional JSON construction for vision requests and focus metadata."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "Visual.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)


for helper, next_marker in (
    ("static int visual_json_object_add_checked(", "static int visual_json_array_add_checked("),
    ("static int visual_json_array_add_checked(", "static int visual_vwebcam_prepare_dir("),
):
    start = source.find(helper)
    end = source.find(next_marker, start + 1)
    require(start >= 0 and end > start, f"checked JSON helper exists: {helper}")
    block = source[start:end]
    require("json_object_put(value)" in block, "failed insertion releases the untransferred JSON value")
    require("!= 0" in block, "JSON insertion return code is checked")

start = source.find("static void publish_focus_region(")
end = source.find("\n/* Return the natural-language description", start)
require(start >= 0 and end > start, "focus metadata writer exists")
focus = source[start:end]
for key in ('"x"', '"y"', '"width"', '"height"', '"label"'):
    require(f"visual_json_object_add_checked(root, {key}" in focus,
            f"focus metadata checks insertion for {key}")
require("json_object_object_add(root" not in focus,
        "focus metadata does not use unchecked JSON insertion")

start = source.find("static char *call_vision_model(")
end = source.find("\nstatic int visual_column_exists(", start)
require(start >= 0 and end > start, "vision request builder exists")
builder = source[start:end]
for key in ('"model"', '"stream"', '"think"', '"format"', '"keep_alive"',
            '"num_ctx"', '"num_predict"', '"temperature"', '"top_p"',
            '"top_k"', '"min_p"', '"presence_penalty"', '"repeat_penalty"',
            '"role"', '"content"', '"images"', '"options"', '"messages"'):
    require(key in builder, f"vision request includes expected field {key}")
require("visual_json_object_add_checked" in builder and
        "visual_json_array_add_checked" in builder,
        "vision request uses checked JSON helpers")
require("goto build_failure;" in builder and "payload_ready:" in builder,
        "vision request aborts construction atomically before serialization")
require("if (root) json_object_put(root);" in builder and
        "if (messages) json_object_put(messages);" in builder and
        "if (sys) json_object_put(sys);" in builder and
        "if (user) json_object_put(user);" in builder and
        "if (images) json_object_put(images);" in builder and
        "if (options) json_object_put(options);" in builder,
        "all partially-owned JSON containers are released on construction failure")
require("headers = curl_slist_append(headers, \"Content-Type: application/json\");\n        if (!headers)" in builder,
        "vision request aborts cleanly if the HTTP header allocation fails")
require("json_object_object_add(root," not in builder and
        "json_object_array_add(messages," not in builder and
        "json_object_array_add(images," not in builder,
        "vision request has no unchecked direct JSON insertions")

print("Vision JSON allocation and ownership smoke passed.")
