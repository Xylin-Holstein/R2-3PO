#!/usr/bin/env python3
"""Contract checks for the actual Gemma 3:4b + Imagination integration."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

core = read("r2.c")
header = read("r2.h")
visual = read("Visual.c")
launcher = read("R2_Launch_Code.sh")
makefile = read("Makefile")
preflight = read("tools/preflight_r2_hardware.sh")
shell = read("shell.c")
imagination = read("Imagination.c")
imagination_header = read("Imagination.h")
workflow = read(".github/workflows/build.yml")

assert re.search(r'^static char MODEL\[256\] = "gemma3:4b";$', core, re.M)
assert 'configure_conversation_model()' in core
assert '#define R2_VISION_DEFAULT_MODEL "gemma3:4b"' in visual
assert 'R2_MODEL="${R2_MODEL:-gemma3:4b}"' in launcher
assert 'R2_VISION_MODEL="$R2_MODEL"' in launcher
assert 'R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"' in launcher
assert 'configure_ollama_endpoint()' in core
assert 'R2_OLLAMA_BASE_URL' in core and 'configure_ollama_endpoint()' in core
assert '127.0.0.1:11435' in launcher
assert 'char *r2_model_generate(const char *system, const char *prompt, int max_tokens)' in core
assert 'r2_model_generate(const char *system, const char *prompt, int max_tokens)' in header
assert 'ollama_chat_limited(&request, 1, system, max_tokens)' in core
assert '"num_predict"' in core
assert 'static long OLLAMA_TIMEOUT_SECONDS = 21600L;' in core
assert 'getenv("R2_OLLAMA_TIMEOUT_SECONDS")' in core
assert 'CURLOPT_TIMEOUT' in core and 'OLLAMA_TIMEOUT_SECONDS' in core
assert 'CURLOPT_CONNECTTIMEOUT, 30L' in core
assert 'static long R2_VISION_TIMEOUT_SECONDS = 21600L;' in visual
assert 'getenv("R2_OLLAMA_TIMEOUT_SECONDS")' in visual
assert 'CURLOPT_TIMEOUT, R2_VISION_TIMEOUT_SECONDS' in visual
assert 'CURLOPT_CONNECTTIMEOUT, 30L' in visual

for api in ("r2_imagination_init", "r2_imagination_shutdown", "r2_imagination_create", "r2_imagination_feedback"):
    assert api in imagination and api in imagination_header
assert 'r2_model_generate(system, prompt, 700)' in imagination
assert 'r2_retrieve_memories(request)' in imagination
assert 'r2_diary_search(request' in imagination
assert 'r2_log_search(request' in imagination
assert 'r2_reality_context()' in imagination
assert 'r2_visual_search(request' in imagination
assert 'r2_reward_context()' in imagination
assert 'r2_addiction_report()' in imagination
assert 'r2_altself_list(6)' in imagination
assert 'r2_altself_create(' in imagination and 'hypothetical only' in imagination
assert 'r2_reward_apply_once(target, "verified_imagination", 1' in imagination
assert 'r2_imagination_init()' in core and 'r2_imagination_shutdown()' in core
assert 'extract_marker(reply, "[IMAGINE]", "[END IMAGINE]"' in core
assert 'extract_marker(reply, "[IMAGINE_FEEDBACK]", "[END IMAGINE_FEEDBACK]"' in core
assert '"IMAGINE", "IMAGINE_FEEDBACK"' in core
assert '[IMAGINE] request [END IMAGINE]' in core
assert 'shell_imagine' in shell and 'shell_conversation(request)' in shell
assert 'Imagination.c' in makefile and 'Imagination.h' in makefile
assert '"$R2_SOURCE/Imagination.c"' in launcher and '"$R2_SOURCE/Imagination.h"' in launcher
assert 'Imagination.c Imagination.h' in preflight
assert 'tests/gemma3_model_smoke.py' in workflow
assert 'tests/qwen35_model_smoke.py' not in workflow
assert 'gemma4:e2b' not in core + header + visual + launcher
print("Gemma 3:4b shared-model and Imagination integration checks passed.")
