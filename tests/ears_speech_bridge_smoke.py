#!/usr/bin/env python3
"""Regression checks for the explicit local microphone-to-chat path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
header = (ROOT / "r2.h").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
ears = (ROOT / "Ears.c").read_text(encoding="utf-8")
ears_header = (ROOT / "Ears.h").read_text(encoding="utf-8")

start = core.index("char *r2_ears_listen(unsigned seconds)")
end = core.index("int r2_ears_start(void)", start)
bridge = core[start:end]

assert "r2_ears_read(ears, pcm, wanted)" in bridge
assert "r2_wav_write_wav_header" not in bridge
assert "r2_ears_write_wav_header(wav, total_bytes)" in bridge
assert '"R2_WHISPER_MODEL"' in bridge and '"R2_WHISPER_CLI"' in bridge
assert 'execlp(cli, cli, "-m", model, "-f", wav_path' in bridge
assert "system(" not in bridge and "popen(" not in bridge
assert "seconds < 1U || seconds > 15U" in bridge
assert "O_EXCL | O_CLOEXEC, 0600" in bridge
assert "r2_ears_close(ears)" in bridge
assert "unlink(wav_path)" in bridge and "unlink(transcript_path)" in bridge
assert "char *r2_ears_listen(unsigned seconds);" in header
assert 'r2_talk(transcript)' in shell
assert 'strncasecmp(argument, "listen", 6)' in shell
assert "ears listen [1-15 seconds]" in shell
assert 'R2_WHISPER_MODEL="${R2_WHISPER_MODEL:-}"' in launcher
assert 'R2_WHISPER_CLI="${R2_WHISPER_CLI:-}"' in launcher
assert "Local Whisper is not configured" in launcher
assert "docs/local-speech-input.md" in launcher
assert "r2_ears_read(" in ears and "ssize_t r2_ears_read(" in ears_header
assert "Qwen 3.5 9B accepts text and images, not raw microphone PCM" in ears_header
print("Local microphone capture, Whisper transcription, and normal chat handoff smoke checks passed.")
