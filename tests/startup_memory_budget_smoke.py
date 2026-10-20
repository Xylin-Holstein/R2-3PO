#!/usr/bin/env python3
"""Protect learned persistent identity from being crowded out of the Ollama prompt."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "r2.c").read_text(encoding="utf-8")

start = source.find("typedef struct {\n    size_t override_budget;")
end = source.find("/* Return a byte prefix ending at a UTF-8 code-point boundary. */", start)
assert start >= 0 and end > start, "could not isolate system-context budget implementation"
budget = source[start:end]

assert "size_t startup_reserve;" in budget
assert "budget.startup_reserve = system_budget / 8;" in budget
assert "if (budget.startup_reserve > 1600)" in budget
assert "budget.startup_reserve = 1600;" in budget
assert "size_t learning_reserve;" in budget
assert "budget.learning_reserve = system_budget / 16;" in budget
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

# The old budget assigned all unreserved bytes to the system override. With
# a full prompt, no bytes remained for priority-5 startup memory. The new
# reserve must be a positive, bounded slice before the override is computed.
for system_budget in (3415, 6200, 12000, 40000):
    startup_reserve = min(system_budget // 8, 1600)
    learning_reserve = min(system_budget // 16, 1000)
    reality = min(system_budget // 8, 3000)
    identity = min(system_budget // 3, system_budget - reality)
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
