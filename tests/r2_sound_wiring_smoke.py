from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
r2 = (ROOT / "r2.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
sounds = (ROOT / "R2Sounds.c").read_text(encoding="utf-8")

checks = {
    "core includes the existing sound API": '#include "R2Sounds.h"' in r2,
    "conversation prompt permits rare state-tagged droid effects": "[R2_SOUND:beep:STATE]" in r2 and "[R2_SOUND:whistle:STATE]" in r2,
    "sound markers are processed before tool handling": r2.find("r2_sounds_process_reply(reply)") < r2.find("char *tools = process_tools(reply)"),
    "launcher compiles the sound module": '"$R2_SOURCE/R2Sounds.c"' in launcher,
    "launcher checks both sound module files": '"$R2_SOURCE/R2Sounds.h"' in launcher,
    "launcher preflights FX access as the runtime user": 'find "$1" -maxdepth 1 -type f -iname "*.mp3"' in launcher and "WARNING: r2 cannot find readable MP3 effects" in launcher,
    "launcher forwards the selected FX directory into the runtime": 'R2_FX_DIR="$R2_FX_DIR"' in launcher,
    "Makefile builds and tracks the sound module": "R2Sounds.c" in makefile and "R2Sounds.h" in makefile,
    "sound selection scans the configured FX directory": "R2_SOUNDS_DEFAULT_FX_DIR" in sounds and "opendir(fx_dir)" in sounds,
    "sound scheduling records an event in the existing Life Log": 'r2_log_event(R2_LOG_SENSORY, "sound_effect_scheduled"' in sounds,
    "core stops/reaps audio playback on shutdown": "r2_sounds_shutdown();" in r2,
}
failed = [label for label, ok in checks.items() if not ok]
for label, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}: {label}")
if failed:
    raise SystemExit(1)
print("R2 sound wiring smoke passed")
