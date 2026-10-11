#!/usr/bin/env python3
"""Guard the shared chat/reflection context against silently dropping a persistent subsystem."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")

start = core.find("static char *append_reality_context(char *base)")
end = core.find("\nint r2_write_diary(void)", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the shared R2 context assembler.")
context = core[start:end]

required_sources = (
    "r2_reality_context()",
    "r2_reward_context()",
    "r2_addiction_report()",
)
for source in required_sources:
    if source not in context:
        raise SystemExit(f"Shared chat/reflection context omits {source}")
for cleanup in ("free(base);", "free(reality);", "free(reward);", "free(addiction);"):
    if cleanup not in context:
        raise SystemExit(f"Shared context does not release {cleanup}")

if core.count("append_reality_context(") < 3:
    raise SystemExit("Shared context is not wired into both reflection and conversation paths.")

for boundary in (
    "Imagined sensory details are predictions, not observations.",
    "Only a real experience or explicit user feedback may update an experiential preference.",
    "[ALTERNATE_CREATE]",
):
    if boundary not in core:
        raise SystemExit(f"Counterfactual reasoning boundary is missing: {boundary}")

if "Only listed stock exists" not in reality:
    raise SystemExit("Authoritative fridge inventory is not represented in Reality context.")

print("shared memory/reality/reward/addiction context wiring smoke passed")
