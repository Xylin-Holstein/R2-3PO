from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
r2 = (ROOT / "r2.c").read_text(encoding="utf-8")

STALE_CLAIMS = (
    "generated automatically",
    "automatically generates one burger",
    "an empty fridge refills with a burger",
)
for source_name, source in (("Reality.c", reality), ("shell.c", shell)):
    lowered = source.lower()
    for claim in STALE_CLAIMS:
        if claim in lowered:
            raise SystemExit(f"{source_name} still contains stale fridge claim: {claim}")

required_rules = (
    "Never infer that food exists merely because R2 is hungry",
    "Only listed stock exists",
    "an empty fridge stays empty",
    "this does not mean a burger exists in inventory",
)
for rule in required_rules:
    if rule.lower() not in reality.lower():
        raise SystemExit(f"Reality.c is missing the explicit fridge rule: {rule}")

if reality.count("!isfinite(f)") < 2 or reality.count("isfinite(parsed)") < 2:
    raise SystemExit("food metric parsing and context must reject non-finite numeric values")

eat_start = r2.find('} else if (nf >= 3 && !strcasecmp(fields[0], "eat")) {')
eat_end = r2.find('} else if (nf >= 4 && !strcasecmp(fields[0], "ratefood")) {', eat_start)
if eat_start < 0 or eat_end < 0:
    raise SystemExit("could not find the world-eat action parser")
eat_action = r2[eat_start:eat_end]
if "atof(fields[2])" in eat_action or '"auto"' not in eat_action:
    raise SystemExit("world eat action must require auto and use authoritative metrics")

print("fridge context consistency smoke passed")
