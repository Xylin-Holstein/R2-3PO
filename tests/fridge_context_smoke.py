from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

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
)
for rule in required_rules:
    if rule.lower() not in reality.lower():
        raise SystemExit(f"Reality.c is missing the explicit fridge rule: {rule}")

print("fridge context consistency smoke passed")
