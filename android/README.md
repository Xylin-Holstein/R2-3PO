# R2 Remote Android App

A dedicated Android companion for the existing R2-3PO process. It is not a second R2 implementation: text turns are passed to the existing `r2_talk()` core function, diary and Life Log views call their current subsystem APIs, and memory search uses R2's existing retrieval function.

## Current implementation

- Native Kotlin + Jetpack Compose app.
- Bearer-token connection settings; encrypted preferences are used when Android's security-crypto provider is available.
- Text chat against the existing R2 core.
- Read-only diary and Life Log views.
- Search existing persistent memories.
- Phone-side Android TextToSpeech for R2's text responses.
- Server status and memory count.
- The gateway is disabled unless `R2_REMOTE_TOKEN` is configured.

## Build

Open the `android/` directory in Android Studio (JDK 17; Android SDK 35), allow Gradle sync, then use **Build > Build Bundle(s) / APK(s) > Build APK(s)**. This repository does not yet include a Gradle wrapper or a tested release APK.

## Connect securely

1. Install Tailscale on the Linux PC and Android phone and sign into the same private tailnet.
2. Generate a random secret of at least 24 characters, for example with `openssl rand -hex 32`.
3. Export `R2_REMOTE_TOKEN` in the environment that launches R2. Optionally set `R2_REMOTE_PORT` (default `8765`).
4. Recompile and launch using `R2_Launch_Code.sh`.
5. In the app, enter `http://<PC-TAILSCALE-IP>:8765` and the same token.
6. Restrict port 8765 with host firewall and tailnet ACLs. Never port-forward this HTTP port to the public internet. For untrusted networks, use an HTTPS reverse proxy on the private network.

The token is a shared bearer secret. Anyone who obtains it can access the gateway, so keep it private and rotate it if exposed.

## API currently implemented

- `GET /api/status`
- `GET /api/conversation?limit=50` (persistent conversation turns shared with the PC shell)
- `POST /api/chat` with JSON `{"message":"..."}`
- `GET /api/diary?limit=20`
- `GET /api/life-log?limit=30`
- `GET /api/memories?query=...`

The current gateway accepts JSON request bodies up to 2 MiB and text messages up to 64 KiB. It is a minimal single-request-at-a-time HTTP/1.1 server, not yet a general-purpose internet-facing web server.

## Not implemented yet

- Photo/video/audio uploads and media-processing pipeline.
- WebSocket push synchronization / durable shared transcript UI.
- Live WebRTC calls, microphone streaming, speech recognition, and streaming R2 TTS.
- Upload quotas, attachment store, and server-side media metadata.

These require additional work; the current app reports this explicitly rather than presenting nonfunctional controls.
