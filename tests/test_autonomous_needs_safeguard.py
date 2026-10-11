#!/usr/bin/env python3
"""Source-contract regression for single-meal autonomous needs cycles."""

from pathlib import Path

source = Path("r2.c").read_text(encoding="utf-8")
start = source.index("static void autonomous_needs_check(void)")
end = source.index("\nint r2_think(void)", start)
function = source[start:end]

assert "int food_action_succeeded = 0;" in function
assert 'strstr(result, "REAL NEEDS RESULT: eating \'") != NULL' in function
assert 'strstr(result, "updated persistent hunger and energy.") != NULL' in function
assert 'strstr(result, "FRIDGE RESULT: eating \'") != NULL' in function
assert 'strstr(result, "reduced hunger and consumed fridge stock.") != NULL' in function

# The deterministic safeguard remains available after model failure/no action,
# but is skipped once the model's own meal has succeeded.
assert "if (!food_action_succeeded)\n        autonomous_needs_food_safeguard();" in function
assert function.count("autonomous_needs_food_safeguard();") == 4, (
    "expected fallback for missing state, allocation failure, model failure, "
    "and the final guarded fallback"
)
print("autonomous needs single-meal smoke passed")
