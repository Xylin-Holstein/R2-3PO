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

The explicit capture duration is limited to 1–15 seconds. Local transcription is
terminated if it exceeds 90 seconds or R2 begins shutting down. If the model
or executable is missing, R2 reports that speech recognition is unavailable
instead of pretending it understood audio.

### Hands-free Watch

At the R2 prompt:

- `watch start` — start the real microphone-to-chat worker. This is the explicit action that enables ongoing microphone capture.
- `watch status` — inspect whether the worker is actually running.
- `watch stop` — cancel capture/transcription and join the worker.
- `vision watch start` — separately enable periodic visual observation; Watch does not open the camera by itself.

Watch uses local voice-activity detection rather than transcribing fixed five-second clips. It waits for speech energy, then ends an utterance after about 600 ms of low-energy audio or at the 15-second maximum. Silence-only windows and utterances Whisper cannot decode do not become chat turns. Every nonempty transcript is passed through the same public `r2_talk()` conversation path as typed messages. The worker checks that the local Whisper model is readable and the CLI executable is available before it starts, and stops after five consecutive capture/ASR failures instead of claiming to remain active.

The VAD start threshold defaults to `600` mean absolute PCM level. If your microphone is unusually quiet or noisy, set `R2_EARS_VAD_THRESHOLD` to a value from 100 to 10000 before launching R2. The launcher passes this setting through to the dedicated `r2` account. Adjust it only after testing with your actual microphone and room noise.

These paths have CI regression checks for capture bounds, cancellation, shared chat handoff, and worker lifecycle. A passing CI build does not prove that a particular microphone, PulseAudio/PipeWire setup, Whisper binary, or model works on the target machine; those still need a local test.


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
