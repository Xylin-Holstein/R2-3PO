from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")

for claim in ("generated automatically", "automatically generates one burger",
              "an empty fridge refills with a burger", "guaranteed fridge meal"):
    if claim in reality.lower():
        raise SystemExit(f"Reality.c still contains a synthetic-fridge claim: {claim}")
if "fridge_seed_if_empty_locked" in reality:
    raise SystemExit("Reality.c still fabricates stock when the fridge is empty")
if 'const char *guaranteed = "burger:' in reality:
    raise SystemExit("Reality.c still injects a synthetic burger metric into context")
if '<food name=\\"burger\\" fullness=\\"100\\"' in reality:
    raise SystemExit("fresh Reality setup still seeds a hard-coded burger metric")
if reality.count("!isfinite(f)") < 2 or reality.count("!isfinite(parsed)") < 2:
    raise SystemExit("food metric parsing and context must reject non-finite numeric values")
if 'fridge_ensure_column("fullness", "REAL NOT NULL DEFAULT 0")' not in reality:
    raise SystemExit("legacy fridge migration must mark unknown fullness as zero, not invent nutrition")
if 'fridge_ensure_column("energy", "REAL NOT NULL DEFAULT 0")' not in reality:
    raise SystemExit("legacy fridge migration must mark unknown energy as zero, not invent nutrition")
if "if (quantity < 1) return -1;" not in reality:
    raise SystemExit("fridge API must reject non-positive stock quantities")
if "existing_quantity > (sqlite3_int64)INT_MAX - quantity" not in reality:
    raise SystemExit("fridge quantity additions must reject overflow without changing inventory")
if "r2_reality_eat(const char *food, double fullness)" not in reality or "(void)fullness;" not in reality:
    raise SystemExit("caller-supplied fullness must not override configured food metrics")
if "fridge stock is not regenerated automatically." not in reality:
    raise SystemExit("fridge context must state that exhausted stock remains exhausted")
for action in ('r2_reality_eat(fields[1], -1.0)',
               'r2_reality_fridge_eat(fields[1], -1.0)',
               'r2_reality_move_item(fields[1], "fridge")'):
    if action not in core:
        raise SystemExit(f"Core WORLD handler is missing {action}")
for boundary in (
    "SENSORY COUNTERFACTUALS",
    "update preferences only after real experience or explicit feedback",
    "[ALTERNATE_CREATE]",
):
    if boundary not in core and boundary not in reality:
        raise SystemExit(f"Counterfactual boundary is missing: {boundary}")
print("fridge context consistency smoke passed")
