#!/usr/bin/env python3
"""Regression checks for the explicit local microphone-to-chat path."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
header = (ROOT / "r2.h").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
ears = (ROOT / "Ears.c").read_text(encoding="utf-8")
ears_header = (ROOT / "Ears.h").read_text(encoding="utf-8")

start = core.index("static char *r2_ears_listen_internal(unsigned seconds, int stop_on_silence,")
end = core.index("char *r2_ears_listen_cancelable(unsigned seconds,", start)
bridge = core[start:end]
wrappers_end = core.index("int r2_ears_start(void)", end)
wrappers = core[end:wrappers_end]

assert "r2_ears_read(ears, pcm, wanted)" in bridge
assert "r2_ears_write_wav_header(wav, captured_bytes)" in bridge
assert 'getenv("R2_WHISPER_MODEL")' in bridge and 'getenv("R2_WHISPER_CLI")' in bridge
assert 'execlp(cli, cli, "-m", model, "-f", wav_path' in bridge
assert "system(" not in bridge and "popen(" not in bridge
assert "seconds < 1U || seconds > 15U" in bridge
assert "O_EXCL | O_CLOEXEC, 0600" in bridge
assert "r2_ears_close(ears)" in bridge
assert "unlink(wav_path)" in bridge and "unlink(transcript_path)" in bridge
assert "r2_ears_pcm_mean_absolute_level" in bridge
assert "silent_frames >= 60U" in bridge
assert "no_speech_frames >= 200U" in bridge
assert "transcript = calloc(1, 1)" in bridge
assert "char *r2_ears_listen(unsigned seconds);" in header
assert "char *r2_ears_listen_cancelable(unsigned seconds," in header
assert "char *r2_ears_listen_utterance_cancelable(unsigned max_seconds," in header
assert "cancelled && cancelled(userdata)" in bridge
assert "return r2_ears_listen_internal(seconds, 0, cancelled, userdata);" in wrappers
assert "return r2_ears_listen_internal(max_seconds, 1, cancelled, userdata);" in wrappers
assert "return r2_ears_listen_cancelable(seconds, NULL, NULL);" in core
assert 'shell_talk(transcript)' in shell
conversation_start = shell.index("static void shell_talk(const char *message)")
conversation_end = shell.index("static void shell_conversation(", conversation_start)
conversation_entry = shell[conversation_start:conversation_end]
assert 'r2_talk(message)' in conversation_entry, (
    "speech transcripts must enter the same shell_talk -> r2_talk path as typed messages"
)
assert re.search(
    r"static void shell_conversation\(const char \*message\)\s*\{[^}]*shell_talk\(message\);",
    shell, re.S
), "typed conversation must use the same shell_talk entry point as speech transcripts"
assert 'strncasecmp(argument, "listen", 6)' in shell
assert "ears listen [1-15 seconds]" in shell
assert 'R2_WHISPER_MODEL="${R2_WHISPER_MODEL:-}"' in launcher
assert 'R2_WHISPER_CLI="${R2_WHISPER_CLI:-}"' in launcher
assert 'R2_EARS_VAD_THRESHOLD="${R2_EARS_VAD_THRESHOLD:-}"' in launcher
assert "Local Whisper is not configured" in launcher
assert "docs/local-speech-input.md" in launcher
assert "r2_ears_read(" in ears and "ssize_t r2_ears_read(" in ears_header
assert "Qwen 3.5 9B accepts text and images, not raw microphone PCM" in ears_header
print("Local microphone capture, Whisper transcription, and normal chat handoff smoke checks passed.")
