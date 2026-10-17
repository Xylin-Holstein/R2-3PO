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

start = core.index("char *r2_ears_listen_cancelable(unsigned seconds,")
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
assert "char *r2_ears_listen_cancelable(unsigned seconds," in header
assert "while (captured_bytes < total_bytes && !shutting_down &&" in bridge
assert "cancelled && cancelled(userdata)" in bridge
cleanup_start = bridge.index("cleanup:")
cleanup_tail = bridge[cleanup_start:]
assert "if (child > 0)" in cleanup_tail and "kill(child, SIGKILL)" in cleanup_tail, (
    "an abnormal Whisper wait/clock failure must not leave shutdown blocked on a child that ignores SIGTERM"
)
assert "if (waited < 0 && errno != EINTR)" in bridge
wait_error_start = bridge.index("if (waited < 0 && errno != EINTR)")
wait_error_end = bridge.index("if (shutting_down ||", wait_error_start)
wait_error_path = bridge[wait_error_start:wait_error_end]
assert "if (errno == ECHILD)" in wait_error_path and "child = -1;" in wait_error_path, (
    "ECHILD must avoid signaling a PID that has already ceased to be our child"
)
assert "else" in wait_error_path and "goto cleanup;" in wait_error_path, (
    "unexpected waitpid errors must preserve the child PID for SIGKILL cleanup"
)
assert not re.search(r"else\s+child\s*=\s*-1\s*;", wait_error_path), (
    "do not discard the live child PID on unexpected waitpid errors"
)
assert "return r2_ears_listen_cancelable(seconds, NULL, NULL);" in core
assert 'shell_talk(transcript)' in shell
conversation_start = shell.index("static void shell_talk_inner(const char *message)")
conversation_end = shell.index("static void shell_talk(const char *message)", conversation_start)
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
assert "Local Whisper is not configured" in launcher
assert "docs/local-speech-input.md" in launcher
assert "r2_ears_read(" in ears and "ssize_t r2_ears_read(" in ears_header
read_start = ears.index("ssize_t r2_ears_read(")
read_end = ears.index("int r2_ears_get_event(", read_start)
read_impl = ears[read_start:read_end]
live_error_start = read_impl.index("if (pa_simple_read(")
live_error_end = read_impl.index("ears->event.timestamp =", live_error_start)
live_error = read_impl[live_error_start:live_error_end]
assert "r2_ears_close(ears);" in live_error, (
    "a failed live microphone read must close its dead stream"
)
assert "if (feof(ears->file_stream))" in read_impl
assert "r2_ears_close(ears);" in read_impl[
    read_impl.index("if (feof(ears->file_stream))"):
    read_impl.index("if (ferror(ears->file_stream))")
], "decoded audio EOF must close the finished stream"
assert "Qwen 3.5 9B accepts text and images, not raw microphone PCM" in ears_header
print("Local microphone capture, Whisper transcription, and normal chat handoff smoke checks passed.")
