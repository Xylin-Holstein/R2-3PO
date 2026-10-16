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

No audio is uploaded to a remote service, and temporary raw audio/transcript
files are removed after processing. The sensory event records that a
transcription occurred; the recognized text is then handled as a normal user
conversation turn, so it follows the existing chat history and memory path.

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

## Watch coordinator

- `watch` or `watch start` starts a background coordinator after validating the
  local Whisper model, CLI, and default microphone.
- Watch captures non-overlapping five-second microphone chunks. A usable
  transcript is sent through the same serialized shell chat path as typed
  text; silence does not create a turn.
- `watch status` reports the coordinator thread, speech input, and visual
  observation states. `watch stop` cancels microphone capture/Whisper work and
  joins the worker; if a user-facing chat turn has already begun, Watch waits
  for that turn to finish rather than corrupting conversation state.
- If Eyes is already open and the visual subsystem is available, Watch attaches
  the periodic visual observer. Watch never opens a camera or media source by
  itself.

This is bounded chunked listening, not yet uninterrupted streaming or
voice-activity-detected turn segmentation. While a chunk is being transcribed
or R2 is generating its reply, subsequent speech may not be captured. That is
a known next step for the coordinator, not something the current implementation
claims to solve.

This wiring is covered by a static CI regression check. A passing CI build
does not prove that a particular microphone, PulseAudio/PipeWire setup, Whisper
binary, or model works on the target machine; those still need a local test.


## Camera device selection

Eyes defaults to `/dev/video0`. If the intended camera or capture device has a
different node, set `R2_CAMERA_DEVICE` before launching R2, for example:

```sh
export R2_CAMERA_DEVICE=/dev/video2
/home/x/R2_Home/R2_Launch_Code.sh
```

The selected V4L2 device must support the current capture mode (640×480 at
approximately 10 frames per second) and be readable/writable by the `r2`
runtime user. Use the hardware preflight to enumerate available devices, then
test `vision see` and a visual question in chat on the actual machine.
