# R2-3PO Android Remote Companion — Architecture and Build Plan

**Working branch:** `feature/android-remote-connection`  
**Base:** `main`  
**Status:** Android debug APK and R2 core compile passed GitHub Actions on commit 94bb954. The branch includes the remote gateway, Android companion, daily text Life Log mirror, diary write serialization, and the V-Webcam-style R2 face popup. WebRTC live calling, live frame transport, and speech transcription remain unimplemented.

## Product goal

Build a dedicated Android app that lets the user communicate with R2-3PO from anywhere while keeping R2's brain and primary data on the home Linux PC. The PC console and Android app must use one shared, persistent conversation—not separate chat histories.

The app must support:
- Text messages and live conversation history.
- Photo upload from camera or gallery.
- Video-file upload.
- Audio-file upload.
- Live audio/video calls with R2, including spoken replies through text-to-speech (TTS).
- Shared attachment and conversation context across PC and phone.

## Architecture principles

1. **Keep the brain local.** Ollama and R2's existing core continue to run on the home PC. Do not expose Ollama's port (11434) directly to the public internet.
2. **Use R2's existing conversation API.** `r2.h` already declares `r2_talk(const char *message)`. The remote path must integrate with the existing R2 core and its memory/tool pipeline, not create a second personality or independent conversation engine.
3. **One canonical transcript.** All clients read/write through one server-side conversation store. Use stable message IDs, timestamps, sender/source metadata, attachment references, delivery/processing state, and request IDs to avoid duplicate messages.
4. **Serialize access to the brain.** Local-console and remote requests must not race through the same conversation state. Add a single request queue/lock or equivalent core-owned coordinator; preserve message order and return each response to the correct request.
5. **Private-by-default networking.** Prefer Tailscale or another authenticated private overlay for first deployment. Do not port-forward an unauthenticated HTTP service. If public access is later needed, require TLS, strong authentication, rate limits, and explicit upload limits.
6. **Attachments are data, not prompts by themselves.** Store media as files with metadata and link each attachment to its message. Validate MIME type, size, duration, and file signatures; use randomized server-side names and never trust client filenames or paths.
7. **Graceful degradation.** Text chat should work even when a vision, transcription, or TTS model is unavailable. Tell the user which capability is unavailable instead of pretending the media was processed.

## Proposed components

### A. PC-side remote gateway

A small service on the Linux PC handles authenticated app connections, transcript synchronization, uploads, and call signaling. It must bridge into the existing R2 runtime rather than starting a second Ollama chat with separate history.

Before implementation, inspect how `r2_init()`, `r2_talk()`, the interactive loop, and shutdown are implemented in `r2.c` and `shell.c`. Choose the least invasive integration that safely invokes the existing core. Do not rewrite `r2.c` wholesale.

The gateway should provide:
- Health/status endpoint.
- Fetch conversation history and updates since a message ID.
- Send a text message and receive the corresponding R2 response.
- Upload/download attachment metadata and bytes.
- WebSocket (or equivalent) events for new messages and processing state.
- WebRTC signaling for live calls.
- Explicit authentication and authorization.

### B. Shared conversation and media store

Use a persistent store on the PC. Inspect the current SQLite schema before deciding whether to extend an existing conversation table or add dedicated tables. Keep chat transcript records distinct from long-term semantic memories and diary entries.

Suggested logical records:
- **messages:** message ID, conversation ID, sender, client/source, text, created time, status, reply-to/request ID.
- **attachments:** attachment ID, message ID, media type, safe storage path, original display name, size, duration, checksum, processing status, optional transcript/description.
- **call sessions:** session ID, start/end time, state, and relevant event references.

Local and remote messages should enter the same core conversation pipeline. A media message should be represented in the transcript with a durable attachment reference, so later turns can refer to the correct image/audio/video.

### C. Android application

Build a dedicated native Android app (Kotlin is the intended starting point) with:
- Chat transcript and live updates.
- Text composer and send state.
- Camera/gallery photo picker.
- Video and audio file picker.
- Upload progress, retry, cancel, and clear error messages.
- Attachment previews and playback.
- Call screen with camera/microphone permission prompts, mute, camera toggle, hang-up, connection status, and spoken-response controls.
- A configurable private gateway address / pairing process; no secrets hardcoded into the APK.

### D. Media understanding

**Photos:** store the original image, then pass it through R2's existing visual pathway where compatible. The repository already exposes visual functions such as `r2_vision_see()` and a separate local vision model. Verify accepted image input paths and formats before wiring uploads into it.

**Uploaded videos:** preserve the original file. Extract representative frames and audio on the PC, then process those through available vision and audio pipelines. Include duration/frame sampling limits and show processing status. Do not claim the model has watched every frame unless it actually has.

**Uploaded audio:** preserve the original file; add local speech-to-text when available and retain a transcript linked to the attachment. Keep transcription separate from the original audio so the user can replay it and R2 can use the transcript as context.

### E. Live video calling and TTS

Use **WebRTC** for real-time camera/microphone media, with the PC gateway handling authenticated signaling. Avoid sending raw video as repeated ordinary chat uploads. Start with a one-to-one call between the Android app and the home PC.

R2's conversational response should be converted to speech using a local TTS engine and streamed or played back to the caller. The implementation must:
- Handle microphone capture and speaker output without feedback loops.
- Provide interrupt/stop-speaking behavior.
- Avoid repeatedly sending the same camera frame to the model at full frame rate. Sample frames at a controlled rate and feed useful visual context to R2.
- Coordinate speech recognition, frame analysis, R2 responses, and TTS so they do not overlap chaotically.
- Clearly indicate when R2 is listening, thinking, speaking, or disconnected.

A practical first version can use push-to-talk or turn-based voice conversation over a WebRTC call, then evolve toward more natural full-duplex conversation. The exact speech-recognition and TTS engines should be selected after checking the PC's installed packages and hardware constraints.

## Implementation phases

### Phase 0 — Inspect and protect the existing core
- Read the complete current `r2.c` and `shell.c` interfaces around initialization, message history, model requests, and shutdown.
- Inspect the SQLite schema and how messages/memories are currently persisted.
- Identify existing TTS or speech output capabilities; do not assume they exist just because Ears supports audio input.
- Record baseline build instructions and compile the unmodified branch.

### Phase 1 — Secure text gateway and shared history
- Implement a small, authenticated PC-side gateway.
- Integrate it with the existing R2 core through a safe single request path.
- Establish canonical persistent transcript and synchronization.
- Verify PC and Android-originated messages appear in the same order and R2 sees the same ongoing context.
- Add concurrency handling and duplicate-request protection.

### Phase 2 — Android chat app
- Create the Android project, connect/pair it to the gateway, render shared history, send text, and receive updates.
- Add connection state, reconnect handling, and useful errors.

### Phase 3 — Photo, video, and audio attachments
- [x] Authenticated raw-media upload with a 50 MiB limit and MIME/signature checks.
- [x] Save uploads under the R2 workspace with generated filenames and restrictive file permissions.
- [x] Connect uploaded images/videos to R2's existing Eyes/vision path and the shared conversation.
- [ ] Add audio transcription and richer audio understanding.
- [ ] Add video frame/audio extraction, media previews, download/history UI, and upload progress.

### Phase 4 — Live WebRTC call with spoken R2 replies
- [ ] Add authenticated call signaling and WebRTC camera/microphone streams.
- [ ] Add live speech recognition and TTS integration on the PC-side call path.
- [ ] Add controlled video-frame sampling, speaking/listening state, interruption, and echo prevention.
- [x] Android can speak completed text replies using the phone's installed TTS engine; this is not a live call.
- [x] Add a V-Webcam-style popup panel to represent R2 in the call UI. The UI is present; it currently waits for a live Eyes frame transport.

### Phase 5 — Security, recovery, and release
- Test large/invalid uploads, interrupted connections, reconnects, concurrent PC/phone messages, and service restarts.
- Verify private-network-only access and authentication.
- Add service startup instructions, Android build instructions, backup/retention guidance, and troubleshooting documentation.
- Produce a debug APK first; generate a release APK only after device testing.

## Acceptance criteria

- A message sent from Android and its reply appear in the same conversation as messages sent from the PC.
- Restarting the gateway does not erase the transcript or attachment links.
- An upload never becomes an unbounded memory allocation or arbitrary filesystem write.
- Images, videos, and audio are actually processed by the relevant local subsystem, or the app reports why they were not.
- A live call can deliver camera/microphone input and play R2's TTS response without exposing Ollama to the public internet.
- Existing R2 behavior and the current `main` branch remain untouched until the new branch is reviewed and merged.

## Current status

Implemented and build-verified on this branch:
- Authenticated remote gateway integrated with R2's existing r2_talk() and Life Log conversation persistence.
- Native Android app with chat, diary, Life Log, recent/searchable memories, uploads, and phone-side TTS for completed responses.
- Photo/video/audio upload handling with 50 MiB cap and MIME/signature validation; visual uploads use the existing vision path when available.
- Daily Life Log text mirror at /home/x/R2_Home/R2_Log/Log[YYYY-MM-DD].txt, alongside the existing SQLite Life Log. The launcher prepares the directory for the dedicated r2 account.
- Diary write serialization and cleaner diary initialization failure handling.
- V-Webcam-style R2 face popup in the Android call UI, matching the existing dark canvas/green status design.

GitHub Actions run 37887831496 passed both the Android debug APK build and the R2 core compile. The APK is published as the r2-remote-debug-apk workflow artifact.

Still not implemented: WebRTC camera/microphone calls, live Eyes-frame delivery into the face popup, PC-side live speech recognition/TTS streaming, audio transcription, continuous push synchronization, media preview/download history, and physical-device testing. The face popup is a UI component, not a claim that a live call is connected.
