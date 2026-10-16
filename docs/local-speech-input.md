# Local speech input for R2 Ears

R2's Ears capture layer reads microphone PCM. The Qwen 3.5 chat endpoint used by
this project accepts text and images, not raw microphone PCM, so microphone
capture alone cannot make spoken words part of a conversation. The explicit
`ears listen` command connects the layers:

1. Capture a bounded 1–15 second microphone utterance as 48 kHz, stereo, 16-bit PCM.
2. Write a temporary WAV in a private `0700` directory.
3. Run a local `whisper.cpp` `whisper-cli` executable against a local GGML Whisper model.
4. Remove the temporary WAV and transcript.
5. Pass the recognized text to the same `r2_talk()` conversation path used by typed messages.

No audio is uploaded to a remote service, and raw audio/transcript files are
not retained after the command. R2 records that a transcription occurred, not
the transcript itself, in the Sensory Journal/Life Log.

## Configure it

Install/build `whisper.cpp` locally and obtain a compatible GGML Whisper model
through your normal trusted process. This feature deliberately does not
download models or use a remote speech API.

Make the model readable by the dedicated Linux user `r2`, and ensure the
Whisper CLI is executable by that user. Then export these variables before
launching R2:

```sh
export R2_WHISPER_MODEL=/home/x/R2_Home/models/ggml-base.en.bin
export R2_WHISPER_CLI=/usr/local/bin/whisper-cli
/home/x/R2_Home/R2_Launch_Code.sh
```

Adjust the paths to match the actual installed model and executable. If
`R2_WHISPER_CLI` is unset, R2 looks for `whisper-cli` on its PATH.
The launcher explicitly passes both variables through to R2's dedicated user.

## Use it

At the R2 prompt:

- `ears listen` — record five seconds, transcribe locally, and send the result to chat.
- `ears listen 8` — record eight seconds.
- `ears status` — inspect whether the raw Ears source is currently open.
- `ears start` / `ears stop` — open/close raw microphone capture; these commands alone do not transcribe speech.

The capture duration is limited to 1–15 seconds. Local transcription is
terminated if it exceeds 90 seconds or R2 begins shutting down. If the model
or executable is missing, R2 reports that speech recognition is unavailable
instead of pretending it understood audio.

This wiring is covered by a static CI regression check. A passing CI build
does not prove that a particular microphone, PulseAudio/PipeWire setup, Whisper
binary, or model works on the target machine; those still need a local test.
