#!/usr/bin/env python3
"""Guard R2's full core prompt and bounded Ollama context limits."""
from pathlib import Path
import ast
import re

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def macro(name: str) -> int:
    match = re.search(rf"^#define\\s+{re.escape(name)}\\s+(\\d+)\\s*$", core, re.MULTILINE)
    if not match:
        raise SystemExit(f"Missing numeric limit: {name}")
    return int(match.group(1))

per_message = macro("OLLAMA_MAX_SYSTEM_MESSAGE_CHARS")
system_total = macro("OLLAMA_MAX_SYSTEM_TOTAL_CHARS")
history_total = macro("OLLAMA_MAX_TOTAL_CHARS")
if per_message < 20000:
    raise SystemExit(f"System message cap too small: {per_message}")
if system_total < 32000:
    raise SystemExit(f"Aggregate system context cap too small: {system_total}")
if history_total < 60000:
    raise SystemExit(f"Bounded total history cap unexpectedly shrank: {history_total}")

start = core.find("static const char *SYSTEM_PROMPT =")
end = core.find(";\\n", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the complete SYSTEM_PROMPT definition")
definition = core[start:end]
literals = re.findall(r'"(?:\\\\.|[^"\\\\])*"', definition)
try:
    prompt = "".join(ast.literal_eval(literal) for literal in literals)
except (SyntaxError, ValueError) as exc:
    raise SystemExit(f"Could not decode SYSTEM_PROMPT string literals: {exc}")
if not prompt:
    raise SystemExit("SYSTEM_PROMPT is empty")
if len(prompt) > per_message:
    raise SystemExit(
        f"SYSTEM_PROMPT has {len(prompt)} characters, exceeding its {per_message}-character cap"
    )
for required in (
    "CONVERSATION AND DIARY ARE DIFFERENT",
    "REAL FILE TOOLS",
    "GENERAL",
):
    if required not in prompt:
        raise SystemExit(f"Complete prompt is missing its expected trailing section: {required}")
print(
    f"system prompt budget smoke passed: prompt={len(prompt)} chars, "
    f"per-message={per_message}, aggregate-system={system_total}, history={history_total}"
)
