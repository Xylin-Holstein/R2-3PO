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

refresh_start = core.find("static int refresh_reality_context_message(void)")
refresh_end = core.find("\nstatic char *r2_talk_impl", refresh_start)
if refresh_start < 0 or refresh_end < 0:
    raise SystemExit("Could not locate live conversation context refresh.")
refresh = core[refresh_start:refresh_end]
if "char *context = r2_reality_context()" not in refresh or "build_learning_context()" not in refresh:
    raise SystemExit("Live chat must refresh Reality and learning context as separate system messages.")
if "append_reality_context(NULL)" in refresh:
    raise SystemExit("Live chat must not combine unbounded Reality and learning context into one truncatable message.")
learning_start = core.find("static char *build_learning_context(void)")
learning_end = core.find("\n/* Refresh Reality and learning", learning_start)
if learning_start < 0 or learning_end < 0:
    raise SystemExit("Could not locate the bounded live learning-context builder.")
learning = core[learning_start:learning_end]
for source in ("r2_reward_context()", "r2_addiction_report()", "PERSISTENT LEARNING CONTEXT"):
    if source not in learning:
        raise SystemExit(f"Live learning context omits {source}")
if "size_t cap = 2400" not in learning or "REWARD/REINFORCEMENT SUMMARY:\\n%.900s" not in learning or "REPEATED-INTEREST SUMMARY:\\n%.900s" not in learning:
    raise SystemExit("Live learning context must fit within the reserved system-prompt budget.")

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

# Startup memory uses a different heading from the identity prompt. It must
# have an explicit serialization priority, after focused live retrieval but
# before learned summaries and archived dialogue.
priority_start = core.find("static int r2_inference_system_priority(const char *content, const char *query)")
priority_end = core.find("\nstatic size_t r2_order_inference_messages", priority_start)
if priority_start < 0 or priority_end < 0:
    raise SystemExit("Could not locate inference system-context priority ordering.")
priority = core[priority_start:priority_end]
if 'strstr(content, "R2 PERSISTENT MEMORY - STARTUP CONTEXT")' not in priority or "return 5;" not in priority:
    raise SystemExit("Startup persistent memory is not explicitly prioritized.")
if "for (int priority = 0; priority < 8" not in core:
    raise SystemExit("Inference serialization does not iterate all six context priorities.")
if 'else if (strstr(content, "R2 PERSISTENT MEMORY - STARTUP CONTEXT"))' not in core:
    raise SystemExit("Serialized context budgeting does not use the startup-memory priority.")

# Every real request should report whether the exact current user text and
# the key context blocks actually survived serialization, without logging text.
if "[R2 Prompt Audit]" not in core or "exact_user_turn=%d" not in core or "query_in_user_turn=%d" not in core:
    raise SystemExit("Serialized prompt audit does not verify the current user turn.")
for field in ("startup_memory=%d", "retrieved_context=%d", "reality_context=%d", "learning_context=%d"):
    if field not in core:
        raise SystemExit(f"Serialized prompt audit omits {field}")
