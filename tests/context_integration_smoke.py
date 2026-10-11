#!/usr/bin/env python3
"""Guard the shared chat/reflection context against silently dropping a persistent subsystem."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
addiction = (ROOT / "Addiction.c").read_text(encoding="utf-8")

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

dream_start = core.find("int r2_sleep_and_dream(double hours)")
dream_end = core.find("static void autonomous_needs_food_safeguard", dream_start)
if dream_start < 0 or dream_end < 0:
    raise SystemExit("Could not locate the private dream context path.")
dream = core[dream_start:dream_end]
for source in ("r2_reward_context()", "r2_addiction_report()", "LEARNED REINFORCEMENT CONTEXT", "REPEATED-INTEREST HISTORY"):
    if source not in dream:
        raise SystemExit(f"Private dream context omits {source}")

for boundary in (
    "Imagined sensory details are predictions, not observations.",
    "Only a real experience or explicit user feedback may update an experiential preference.",
    "[ALTERNATE_CREATE]",
):
    if boundary not in core:
        raise SystemExit(f"Counterfactual reasoning boundary is missing: {boundary}")

if "Only listed stock exists" not in reality:
    raise SystemExit("Authoritative fridge inventory is not represented in Reality context.")
if "r2_reality_self_facts ORDER BY updated_at DESC LIMIT 40" not in reality:
    raise SystemExit("Self-fact context is not bounded for long-running sessions.")
if "ORDER BY satisfaction_sum*1.0/rating_count DESC LIMIT 24" not in reality:
    raise SystemExit("Ingredient-preference context is not bounded.")
if "older facts remain stored" not in reality or "full history remains stored" not in reality:
    raise SystemExit("Bounded context must disclose that older persistent facts remain in storage.")

if "ORDER BY t.updated_at DESC LIMIT 17" not in addiction or "if (rows >= 16)" not in addiction:
    raise SystemExit("Addiction context must remain bounded for long-running prompt stability.")
if "Additional repeated-interest records omitted from chat context" not in addiction:
    raise SystemExit("Bounded addiction context must disclose omitted history without deleting it.")

print("shared memory/reality/reward/addiction context wiring smoke passed")
