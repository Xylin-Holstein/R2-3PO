from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
r2 = (ROOT / "r2.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
sounds = (ROOT / "R2Sounds.c").read_text(encoding="utf-8")
sound_header = (ROOT / "R2Sounds.h").read_text(encoding="utf-8")
reply_module = (ROOT / "R2Reply.c").read_text(encoding="utf-8")
talk_start = r2.find("static char *r2_talk_impl(const char *message)")
talk_end = r2.find("\nchar *r2_talk(const char *message)", talk_start)
talk = r2[talk_start:talk_end] if talk_start >= 0 and talk_end > talk_start else ""

checks = {
    "core includes the sound API": '#include "R2Sounds.h"' in r2,
    "prompt permits optional state-aware droid sounds": "[R2_SOUND:beep:STATE]" in r2 and "[R2_SOUND:whistle:STATE]" in r2,
    "need-related sound tags are grounded in Reality": "Do not infer hunger or sleepiness from the topic alone" in r2 and "current Reality state supports them" in r2,
    "tool routing happens before sound playback": talk.find("process_tools(tool_input") >= 0 and talk.find("r2_sounds_process_reply(reply)") > talk.find("process_tools(tool_input"),
    "sound markers are stripped before tool side effects": talk.find("r2_sounds_strip_markers(reply)") >= 0 and talk.find("r2_sounds_strip_markers(reply)") < talk.find("process_tools(tool_input"),
    "official FX location is the default": '/home/x/R2_Home/R2_sounds/FX' in sound_header,
    "launcher passes the official FX location into R2": 'R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"' in launcher and 'R2_FX_DIR="$R2_FX_DIR"' in launcher,
    "reply sanitizer removes private reasoning blocks": 'r2_reply_sanitize' in reply_module and '"<think>"' in reply_module and '"<analysis>"' in reply_module,
    "core sanitizes generated user-facing replies": 'r2_reply_sanitize(reply)' in talk,
    "launcher compiles the reply sanitizer module": '"$R2_SOURCE/R2Reply.c"' in launcher,
    "Makefile builds and tracks reply sanitizer": "R2Reply.c" in makefile and "R2Reply.h" in makefile,
    "launcher compiles and checks the sound module": '"$R2_SOURCE/R2Sounds.c"' in launcher and '"$R2_SOURCE/R2Sounds.h"' in launcher,
    "sound selection scans the configured FX directory": "R2_SOUNDS_DEFAULT_FX_DIR" in sounds and "opendir(fx_dir)" in sounds,
    "sound scheduling records an event in the Life Log": 'r2_log_event(R2_LOG_SENSORY, "sound_effect_scheduled"' in sounds,
    "core shuts down sound playback": "r2_sounds_shutdown();" in r2,
    "core initializes the sound lifecycle": "r2_sounds_init();" in r2 and "void r2_sounds_init(void)" in sounds,
    "sound marker recognition is case-insensitive": "find_sound_marker" in sounds and "strncasecmp(p, marker, marker_len)" in sounds,
    "tool output append clamps snprintf lengths before copying": "if (_append_n >= sizeof(_b))" in r2 and "memcpy(results + len, _b, _append_n)" in r2 and "[tool output truncated]" in r2 and "memcpy(results + len, _b, (size_t)_n)" not in r2,
    "reply sanitizer rechecks private text exposed after hidden tags": "visible_looks_private" in reply_module and "Removing a tagged private block" in reply_module,
}
failed = [label for label, ok in checks.items() if not ok]
for label, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}: {label}")
if failed:
    raise SystemExit(1)
print("R2 sound wiring smoke passed")
