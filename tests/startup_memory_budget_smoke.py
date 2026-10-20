#!/usr/bin/env python3
"""Protect learned persistent identity from being crowded out of the Ollama prompt."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "r2.c").read_text(encoding="utf-8")

start = source.find("typedef struct {\n    size_t override_budget;")
end = source.find("/* Return a byte prefix ending at a UTF-8 code-point boundary. Prompt", start)
assert start >= 0 and end > start, "could not isolate system-context budget implementation"
budget = source[start:end]

assert "size_t startup_reserve;" in budget
assert "budget.reality_reserve = system_budget / 10;" in budget
assert "budget.identity_budget = system_budget / 4;" in budget
assert "budget.startup_reserve = system_budget / 10;" in budget
assert "if (budget.startup_reserve > 1600)" in budget
assert "budget.startup_reserve = 1600;" in budget
assert "size_t learning_reserve;" in budget
assert "budget.learning_reserve = system_budget / 10;" in budget
assert "if (budget.learning_reserve > 1000)" in budget
assert "budget.learning_reserve = 1000;" in budget
assert "budget.startup_reserve +" in budget
assert "budget.learning_reserve;" in budget
assert "system_budget - reserved" in budget
assert "system_budget / 4" in budget

priority_start = source.find("static int r2_inference_system_priority(")
priority_end = source.find("\nstatic size_t r2_order_inference_messages", priority_start)
assert priority_start >= 0 and priority_end > priority_start
priority = source[priority_start:priority_end]
assert 'strstr(content, "R2 PERSISTENT MEMORY - STARTUP CONTEXT")' in priority
assert '"PERSISTENT LEARNING CONTEXT"' in priority
assert "return 5;" in priority and "return 6;" in priority
assert '"RECENT PERSISTED CONVERSATION CONTEXT"' in priority
assert "return 7;" in priority

startup_start = source.find("snprintf(\n        startup_context,")
startup_end = source.find("\n    );", startup_start)
assert startup_start >= 0 and startup_end > startup_start
startup_builder = source[startup_start:startup_end]
assert "newest %d records;" in startup_builder
assert "This startup memory does NOT represent the entirety" not in startup_builder
assert "New memories written during this session can be retrieved" not in startup_builder

# The startup block is later truncated at a record boundary. Order durable
# identity/person/relationship categories before miscellaneous recent facts so
# a burst of newer low-value records cannot erase learned continuity.
startup_fn_start = source.find("static char *get_startup_memories(")
startup_sql_start = source.find("const char *sql =", startup_fn_start)
startup_sql_end = source.find(";\\n", startup_sql_start)
assert startup_fn_start >= 0 and startup_sql_start > startup_fn_start and startup_sql_end > startup_sql_start
startup_sql = source[startup_sql_start:startup_sql_end]
for clause in (
    "ORDER BY CASE lower(category)",
    "WHEN 'self' THEN 0",
    "WHEN 'identity' THEN 0",
    "WHEN 'relationship' THEN 1",
    "WHEN 'person' THEN 2",
    "updated_at DESC",
    "LIMIT ?",
):
    assert clause in startup_sql, f"startup memory ordering omits {clause!r}"

# Verify the context builder still connects the separate durable stores; budget
# changes must not substitute a prompt-only identity shortcut for DB evidence.
learning_start = source.find("static char *build_learning_context(void)")
learning_end = source.find("\n/* Refresh Reality and learning", learning_start)
assert learning_start >= 0 and learning_end > learning_start
learning_builder = source[learning_start:learning_end]
assert "r2_reward_context()" in learning_builder
assert "r2_addiction_report()" in learning_builder
assert "PERSISTENT LEARNING CONTEXT" in learning_builder

refresh_start = source.find("static int refresh_reality_context_message(void)")
refresh_end = source.find("\n}", refresh_start)
assert refresh_start >= 0 and refresh_end > refresh_start
refresh = source[refresh_start:refresh_end]
assert "r2_reality_context()" in refresh
assert "build_learning_context()" in refresh
assert 'message_add_pair_locked("system", context' in refresh

# The old budget assigned all unreserved bytes to the system override. With
# a full prompt, no bytes remained for priority-5 startup memory. The new
# reserve must be a positive, bounded slice before the override is computed.
for system_budget in (3415, 6200, 12000, 40000):
    startup_reserve = min(system_budget // 10, 1600)
    learning_reserve = min(system_budget // 10, 1000)
    reality = min(system_budget // 10, 3000)
    identity = min(system_budget // 4, system_budget - reality)
    retrieval = system_budget // 8
    capabilities = system_budget // 16
    reserved = (reality + identity + retrieval + capabilities +
                startup_reserve + learning_reserve)
    override = system_budget - reserved if system_budget > reserved else system_budget // 4
    assert startup_reserve > 0 and learning_reserve > 0
    assert override < system_budget - (
        reality + identity + retrieval + capabilities + startup_reserve
    )
print("Startup persistent-memory prompt budget regression passed.")
