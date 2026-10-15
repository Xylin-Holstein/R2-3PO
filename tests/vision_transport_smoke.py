#!/usr/bin/env python3
"""Guard vision HTTP setup, response bounds, and failure handling."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "Visual.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


write_start = source.find("static size_t response_write(")
write_end = source.find("\nstatic int mkdir_one(", write_start)
if write_start < 0 or write_end <= write_start:
    raise SystemExit("FAIL: could not isolate response_write()")
write_fn = source[write_start:write_end]
require("b->length > R2_VISION_MAX_RESPONSE" in write_fn
        and "amount > R2_VISION_MAX_RESPONSE - b->length" in write_fn,
        "response size arithmetic checks the existing length before subtraction")

start = source.find("static char *call_vision_model(")
end = source.find("\nstatic int visual_column_exists(", start)
if start < 0 or end <= start:
    raise SystemExit("FAIL: could not isolate call_vision_model()")
vision = source[start:end]

require("if (!curl)" in vision and '"vision_curl_init_failed"' in vision,
        "curl initialization failure is visible and sends no request")
require("headers = curl_slist_append(NULL, \"Content-Type: application/json\");" in vision
        and "if (!headers)" in vision
        and '"vision_http_header_allocation_failed"' in vision,
        "header allocation failure aborts before setting up or sending the request")
setup = vision.find("CURLcode setup = CURLE_OK;")
setup_fail = vision.find("if (setup != CURLE_OK)", setup)
perform = vision.find("curl_easy_perform(curl)")
require(setup >= 0 and setup_fail > setup and perform > setup_fail
        and "R2_VISION_SETOPT(CURLOPT_POSTFIELDSIZE_LARGE" in vision
        and "R2_VISION_SETOPT(CURLOPT_XFERINFOFUNCTION, vision_progress)" in vision,
        "every required curl option is checked before network I/O")
require('"vision_curl_setup_failed"' in vision
        and '"vision_response_parse_failed"' in vision,
        "transport setup and malformed successful responses have distinct diagnostics")
require("r2_ollama_vision_request_begin()" in vision
        and "r2_ollama_vision_request_end();" in vision,
        "the shared model request gate remains paired around actual HTTP execution")

print("Vision transport smoke test passed.")
