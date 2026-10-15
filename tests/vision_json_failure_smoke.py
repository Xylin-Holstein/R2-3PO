#!/usr/bin/env python3
"""Guard against malformed/partial vision requests and focus metadata."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "Visual.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


require("static int visual_json_object_add_checked(" in source
        and "static int visual_json_array_add_checked(" in source,
        "vision object and array insertions have checked helpers")
require("json_object_object_add(object, key, value) != 0" in source
        and "json_object_array_add(array, value) != 0" in source
        and "json_object_put(value);" in source,
        "failed JSON insertion releases the value rather than leaking it")

focus_start = source.find("static void publish_focus_region(")
focus_end = source.find("\nstatic ", focus_start + 1)
if focus_start < 0 or focus_end <= focus_start:
    raise SystemExit("FAIL: could not isolate publish_focus_region()")
focus = source[focus_start:focus_end]
require("visual_json_object_add_checked(root, \"x\"" in focus
        and "visual_json_object_add_checked(root, \"height\"" in focus
        and "visual_json_object_add_checked(root, \"label\"" in focus,
        "focus metadata fields are all checked")
require("(void)unlink(tmp);" in focus and "(void)unlink(path);" in focus,
        "failed focus serialization removes temporary and stale focus files")

start = source.find("static char *call_vision_model(")
end = source.find("\nstatic ", start + 1)
if start < 0 or end <= start:
    raise SystemExit("FAIL: could not isolate call_vision_model()")
vision = source[start:end]
require("R2_VISION_ADD_OBJECT(root, \"model\"" in vision
        and "R2_VISION_ADD_OBJECT(root, \"think\"" in vision
        and "R2_VISION_ADD_OBJECT(root, \"options\", options)" in vision,
        "vision model metadata and generation options are checked")
require("R2_VISION_ADD_ARRAY(images, json_object_new_string(image_base64))" in vision
        and "R2_VISION_ADD_OBJECT(user, \"images\", images)" in vision
        and "R2_VISION_ADD_ARRAY(messages, sys)" in vision
        and "R2_VISION_ADD_ARRAY(messages, user)" in vision
        and "R2_VISION_ADD_OBJECT(root, \"messages\", messages)" in vision,
        "image bytes and both chat messages must attach successfully")
require("vision_json_build_failed:" in vision
        and "if (root) json_object_put(root);" in vision
        and "if (images) json_object_put(images);" in vision
        and "if (options) json_object_put(options);" in vision
        and "free(system_prompt);" in vision,
        "failed request construction cleans up every object not yet transferred")
require('json_object_object_add(root, "model", json_object_new_string(model))' not in vision
        and 'json_object_array_add(images, json_object_new_string(image_base64))' not in vision,
        "old unchecked vision payload construction is absent")

print("Vision JSON failure smoke test passed.")
