from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
r2 = (ROOT / "r2.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
sounds = (ROOT / "R2Sounds.c").read_text(encoding="utf-8")
sound_header = (ROOT / "R2Sounds.h").read_text(encoding="utf-8")
reply_module = (ROOT / "R2Reply.c").read_text(encoding="utf-8")

marker_processing_index = r2.find("r2_sounds_process_reply(reply)")
tool_processing_index = r2.find("char *tools = process_tools(reply)")
strip_marker_index = r2.find("r2_sounds_strip_markers(reply)")

checks = {
    "core includes the existing sound API": '#include "R2Sounds.h"' in r2,
    "conversation prompt permits rare state-tagged droid effects": "[R2_SOUND:beep:STATE]" in r2 and "[R2_SOUND:whistle:STATE]" in r2,
    "sound-state hints must be grounded in current evidence": "only when supported by this turn or current Reality evidence" in r2 and "Prefer no marker when the fit is unclear" in r2,
    "need-related sound tags cannot be inferred from topic alone": "Do not infer a need from the topic alone" in r2 and "Use sleepy or hungry only when the current modeled need actually supports it" in r2,
    "tool routing is decided before playback": marker_processing_index >= 0 and tool_processing_index >= 0 and tool_processing_index < marker_processing_index,
    "intermediate tool replies strip markers without playing sounds": strip_marker_index >= 0 and tool_processing_index < strip_marker_index and "r2_sounds_strip_markers" in sound_header,
    "official FX location is hard-coded in the sound API": '/home/x/R2_Home/R2_sounds/FX' in sound_header,
    "launcher uses the official FX location as its default": '/home/x/R2_Home/R2_sounds/FX' in launcher,
    "reply sanitizer removes explicit private reasoning blocks": 'r2_reply_sanitize' in reply_module and '"<think>"' in reply_module and '"<analysis>"' in reply_module,
    "core sanitizes generated user-facing replies": 'r2_reply_sanitize(reply)' in r2,
    "launcher compiles the reply sanitizer module": '"$R2_SOURCE/R2Reply.c"' in launcher,
    "Makefile builds and tracks reply sanitizer": "R2Reply.c" in makefile and "R2Reply.h" in makefile,
    "launcher compiles the sound module": '"$R2_SOURCE/R2Sounds.c"' in launcher,
    "launcher checks both sound module files": '"$R2_SOURCE/R2Sounds.h"' in launcher,
    "launcher preflights FX access as the runtime user": 'find "$1" -maxdepth 1 -type f -iname "*.mp3"' in launcher and "WARNING: r2 cannot find readable MP3 effects" in launcher,
    "launcher forwards the selected FX directory into the runtime": 'R2_FX_DIR="$R2_FX_DIR"' in launcher,
    "launcher runs the read-only sound inventory as r2 when deployed": 'sudo -u r2 python3 "$SOUND_AUDIT" --root "$R2_FX_DIR"' in launcher,
    "Makefile builds and tracks the sound module": "R2Sounds.c" in makefile and "R2Sounds.h" in makefile,
    "sound selection scans the configured FX directory": "R2_SOUNDS_DEFAULT_FX_DIR" in sounds and "opendir(fx_dir)" in sounds,
    "sound scheduling records an event in the existing Life Log": 'r2_log_event(R2_LOG_SENSORY, "sound_effect_scheduled"' in sounds,
    "core stops/reaps audio playback on shutdown": "r2_sounds_shutdown();" in r2,
    "core resets sound lifecycle during initialization": "r2_sounds_init();" in r2 and "void r2_sounds_init(void)" in sounds,
    "sound control marker recognition is case-insensitive": "find_sound_marker" in sounds and "strncasecmp(p, marker, marker_len)" in sounds,
    "marker cleanup fails closed instead of exposing control text": "Could not safely remove sound control markers" in r2 and "if (!sound_clean_reply)" in r2,
}
failed = [label for label, ok in checks.items() if not ok]
for label, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}: {label}")
if failed:
    raise SystemExit(1)
print("R2 sound wiring smoke passed")
