#!/usr/bin/env python3
"""Guard the actual shared runtime context and Imagination integration."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
addiction = (ROOT / "Addiction.c").read_text(encoding="utf-8")
reward = (ROOT / "Reward.c").read_text(encoding="utf-8")
alternate = (ROOT / "AlternateSelf.c").read_text(encoding="utf-8")
imagination = (ROOT / "Imagination.c").read_text(encoding="utf-8")
imagination_header = (ROOT / "Imagination.h").read_text(encoding="utf-8")

start = core.find("static void append_runtime_context(char **context)")
end = core.find("\nstatic char *sanitize_user_facing_reply", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the current runtime context assembler.")
context = core[start:end]
for source in ("r2_reality_context()", "r2_reward_context()", "r2_addiction_report()", "r2_altself_list(5)"):
    if source not in context:
        raise SystemExit(f"Shared runtime context omits {source}")
if "append_runtime_context_section(context" not in context:
    raise SystemExit("Runtime context sections are not assembled through the bounded append helper.")

for source in ("r2_reality_item_memory", "r2_food_experiences",
               "r2_food_preferences", "r2_food_ingredient_preferences",
               "r2_money_account", "r2_reality_self_facts"):
    if source not in reality:
        raise SystemExit(f"Persistent Reality state omits {source}")
if "r2_addiction_report" not in addiction or "r2_addiction_init" not in addiction:
    raise SystemExit("Addiction persistence/report API is missing")
if "r2_reward_context" not in reward or "r2_reward_init" not in reward:
    raise SystemExit("Reward persistence/context API is missing")
if "r2_altself_list" not in alternate or "r2_altself_init" not in alternate:
    raise SystemExit("Alternate-Self storage/context API is missing")

for api in ("r2_imagination_init", "r2_imagination_shutdown",
            "r2_imagination_create", "r2_imagination_feedback"):
    if api not in imagination or api not in imagination_header:
        raise SystemExit(f"Imagination public API is incomplete: {api}")
if "r2_model_generate(system, prompt, 700)" not in imagination:
    raise SystemExit("Imagination is not using the shared configured conversation model.")
for source in ("r2_retrieve_memories(request)", "r2_diary_search(request",
               "r2_log_search(request", "r2_reality_context()",
               "r2_visual_search(request", "r2_reward_context()",
               "r2_addiction_report()", "r2_altself_list(6)"):
    if source not in imagination:
        raise SystemExit(f"Imagination omits relevant existing experience: {source}")
if "hypothetical only" not in imagination or "r2_reward_apply_once(target, \"verified_imagination\", 1" not in imagination:
    raise SystemExit("Imagination must remain counterfactual and reward only evidence-backed accuracy.")
if 'extract_marker(reply, "[IMAGINE]", "[END IMAGINE]"' not in core:
    raise SystemExit("The conversation tool router does not execute Imagination requests.")
if 'extract_marker(reply, "[IMAGINE_FEEDBACK]", "[END IMAGINE_FEEDBACK]"' not in core:
    raise SystemExit("The conversation tool router does not execute Imagination feedback.")
if '"IMAGINE", "IMAGINE_FEEDBACK"' not in core:
    raise SystemExit("Imagination action markers are not stripped from user-facing replies.")
if "r2_imagination_init()" not in core or "r2_imagination_shutdown()" not in core:
    raise SystemExit("Imagination is not connected to core initialization and shutdown.")

print("Shared Reality/reward/addiction/Choice Lab/Imagination context integration smoke passed.")
