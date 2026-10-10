#!/usr/bin/env python3
"""Exercise the shared audio model API against a local, validating Ollama stub."""
import base64
import io
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]
received = {"payload": None, "error": None}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            if self.path != "/api/chat":
                raise AssertionError(f"unexpected path: {self.path}")
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length))
            if payload.get("model") != "gemma4:e2b":
                raise AssertionError(f"wrong model: {payload.get('model')}")
            options = payload.get("options", {})
            if options.get("num_ctx") != 8192 or options.get("num_batch") != 256:
                raise AssertionError(f"shared inference options missing: {options}")
            messages = payload.get("messages", [])
            user_messages = [m for m in messages if m.get("role") == "user"]
            if len(user_messages) != 1:
                raise AssertionError(f"expected one user message: {messages}")
            audio_items = user_messages[0].get("images")
            if not isinstance(audio_items, list) or len(audio_items) != 1:
                raise AssertionError("WAV audio was not attached through the multimodal images field")
            wav_bytes = base64.b64decode(audio_items[0], validate=True)
            if wav_bytes[:4] != b"RIFF" or wav_bytes[8:12] != b"WAVE":
                raise AssertionError("payload did not contain a RIFF/WAVE file")
            with wave.open(io.BytesIO(wav_bytes), "rb") as audio:
                if audio.getframerate() != 16000 or audio.getnchannels() != 1 or audio.getsampwidth() != 2:
                    raise AssertionError(
                        f"unexpected WAV format: rate={audio.getframerate()}, "
                        f"channels={audio.getnchannels()}, width={audio.getsampwidth()}"
                    )
                if audio.getnframes() < 1:
                    raise AssertionError("WAV contained no audio frames")
            received["payload"] = payload
            response = json.dumps({
                "model": "gemma4:e2b",
                "message": {"role": "assistant", "content": "TRANSCRIPT: fixture audio"},
                "done": True,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
        except Exception as exc:
            received["error"] = str(exc)
            response = json.dumps({"error": str(exc)}).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

    def log_message(self, *_args):
        pass


def main():
    try:
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    except OSError as exc:
        raise SystemExit(f"cannot start local Ollama test stub: {exc}")
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        with tempfile.TemporaryDirectory(prefix="r2-audio-payload-") as temp:
            temp = pathlib.Path(temp)
            wav_path = temp / "fixture.wav"
            with wave.open(str(wav_path), "wb") as audio:
                audio.setnchannels(1)
                audio.setsampwidth(2)
                audio.setframerate(16000)
                audio.writeframes(b"\x00\x00" * 16000)

            binary = temp / "audio_model_payload_smoke"
            sources = [
                "tests/audio_model_payload_smoke.c",
                "shell.c", "r2_diary.c", "Log.c", "Reality.c", "Addiction.c",
                "Reward.c", "AlternateSelf.c", "Imagination.c", "Visual.c",
                "Ears.c", "Eyes.c",
            ]
            command = [
                os.environ.get("CC", "cc"),
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-I.",
                f'-DOLLAMA_URL="http://127.0.0.1:{port}/api/chat"',
                *sources, "-o", str(binary),
                "-lcurl", "-lsqlite3", "-lpthread", "-ljson-c",
                "-lpulse-simple", "-lpulse", "-lm",
            ]
            subprocess.run(command, cwd=ROOT, check=True)
            result = subprocess.run(
                [str(binary), str(wav_path)], cwd=ROOT,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=30,
            )
            if result.returncode:
                raise SystemExit(result.stderr or result.stdout or "audio model smoke failed")
            if received["error"]:
                raise SystemExit(received["error"])
            if received["payload"] is None:
                raise SystemExit("the fake Ollama server did not receive an audio payload")
            print(result.stdout.strip())
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
