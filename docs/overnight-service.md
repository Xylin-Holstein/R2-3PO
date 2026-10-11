# Running R2-3PO overnight with systemd

R2 already has a headless shell mode, a supervised systemd unit, a local-model preflight, a private 1999 clock, and a sleep-inhibitor attempt. Use the existing deployment files below; do not create a second service or run the interactive launcher alongside it.

- Runner: `tools/r2_service_runner.sh`
- Unit: `deploy/r2-3po.service`
- Installer: `tools/install_r2_overnight.sh`

The overnight service uses the same persistent databases as the desktop launcher. **Only one R2 core may run at a time.** The installer refuses to rebuild while an R2 process is already running, checks database paths and permissions, takes a private SQLite snapshot, requires a clean integrity audit, builds to a process-specific temporary executable, preserves the previous binary, and only then enables/starts the service. If activation or core initialization fails, it attempts to restore and restart the previous executable.

## One-time host preparation

Run these on the R2 Linux machine, not on GitHub.

Before installing or starting the service, run the non-destructive hardware preflight from your normal desktop account:

```sh
bash /home/x/R2_Home/R2/tools/preflight_r2_hardware.sh
```

It checks the source/deployment files, the local Ollama API and `gemma3:4b`, read-only SQLite integrity, the separate FX folder, and available audio/camera/TV/emulator dependencies. `[FAIL]` items block readiness; `[WARN]` items identify optional hardware or GUI features that need attention. The report does not install packages, change databases, rebuild R2, or start/stop the service. It distinguishes the desktop account's audio session from the `r2` account's default PulseAudio/PipeWire connection and checks whether `r2` can access detected camera and ALSA device nodes when non-interactive `sudo` permits verification. A warning means access was unavailable or could not be proven; it does not itself repair permissions. The interactive launcher adds the host's `audio` group only to the R2 process's supplementary groups when that group exists, preserving the account's existing groups and avoiding a permanent account-membership change. The overnight systemd unit uses the same service-scoped audio group. This allows direct ALSA access where available, but it does not grant access to the desktop user's PulseAudio/PipeWire socket. The final speaker, microphone, and camera test must still be performed from the actual R2 launch environment.

1. Confirm the repository is deployed at `/home/x/R2_Home/R2`. The sound assets remain a separate local directory at `/home/x/R2_Home/R2_sounds/FX`; that directory is not supposed to exist in GitHub.
2. Confirm the dedicated Linux account exists: `id r2`.
3. Install the required build and runtime packages if missing:

   ```sh
   sudo apt-get install -y build-essential libcurl4-openssl-dev libsqlite3-dev libjson-c-dev libpulse-dev libfaketime curl python3 ffmpeg vlc python3-vlc xdotool pulseaudio-utils python3-tk mpg123
   ```

   The R2 launcher and installer do not run `apt-get update` automatically.

4. Start Ollama and ensure the exact local model is installed:

   ```sh
   sudo systemctl enable --now ollama
   ollama pull gemma3:4b
   curl -fsS http://127.0.0.1:11434/api/tags
   ```

   If Ollama is installed without a system unit named `ollama.service`, start it using the host's existing method. The R2 runner waits up to 60 seconds for the local API/model, then uses `tools/preflight_ollama_model.py` to require one real `gemma3:4b` chat completion (up to 180 seconds) before starting the core. This is a localhost-only inference check; it does not download model weights or contact an external API.

5. Confirm the service account can access the existing workspace and local FX assets. Do not solve permission problems with broad `chmod 777` permissions:

   ```sh
   sudo -u r2 test -x /home/x/R2_Home && echo "workspace traversable"
   sudo -u r2 test -r /home/x/R2_Home/R2/r2.c && echo "source readable"
   sudo -u r2 test -w /home/x/R2_Home/R2 && echo "persistent data directory writable"
   sudo -u r2 test -r /home/x/R2_Home/R2_sounds/FX && echo "FX directory readable"
   ```

   If a check fails, correct the existing owner/group/ACL deliberately before continuing. The core stores its five SQLite databases under `R2/` (`r2_memory.db`, `r2_reality.db`, `r2_fridge.db`, `r2_rewards.db`, and `r2_addictions.db`), so account `r2` needs read/write access to that directory and its existing database files.

## Local inference timeout

Foreground chat and its corrective retry allow up to four hours per request by default to accommodate very slow local inference. Override the setting before starting the interactive launcher if needed:

```sh
export R2_OLLAMA_TIMEOUT_SECONDS=14400
/home/x/R2_Home/R2_Launch_Code.sh
```

The launcher forwards this setting to the dedicated `r2` process. For the overnight systemd service, use a service override instead because systemd does not inherit an interactive shell's exported variables:

```sh
sudo systemctl edit r2-3po.service
```

Add these lines in the editor:

```ini
[Service]
Environment=R2_OLLAMA_TIMEOUT_SECONDS=14400
```

Then apply the override and restart the service:

```sh
sudo systemctl daemon-reload
sudo systemctl restart r2-3po.service
```

Accepted values are 30–21,600 seconds (30 seconds to six hours); invalid values use the four-hour default. This only changes how long R2 waits for a request, not inference speed.

## Diagnose a conversation that answers the wrong message

If R2 produces a reply that changes what you said, enable the opt-in prompt trace for one reproduction. This captures the **exact JSON request sent to Ollama**, including message roles/order, the active user message, system instructions, and retrieved memories. It does not change prompt content or model behavior; it is diagnostic evidence for finding the source of a bad response.

First stop the overnight service and any other R2 process, then run the interactive launcher with tracing enabled:

```sh
sudo systemctl stop r2-3po.service
export R2_OLLAMA_TRACE_PROMPTS=1
/home/x/R2_Home/R2_Launch_Code.sh
```

Send one test message that demonstrates the failure. The terminal prints the private trace filename, normally under `/home/x/R2_Home/R2_Log/`. After that one reproduction, exit R2 and disable tracing:

```sh
unset R2_OLLAMA_TRACE_PROMPTS
```

Tracing is off by default. When enabled, each serialized request is saved to a new file with owner-only permissions (`0600`); existing files are never overwritten and symlinks are not followed. The trace contains private conversation history, retrieved memories, and your exact message. **Review and redact it before sharing**; do not post it publicly or leave tracing enabled during ordinary use. Once the trace is available, compare the actual current user message and preceding context against the generated reply to determine whether the distortion came from prompt assembly/context, the model itself, or response handling. Do not treat a successful build as proof that this semantic failure is resolved.

## Build, audit, and activate

First exit any interactive R2 session. The service and interactive launcher share persistent SQLite databases.

Run the existing installer:

```sh
sudo bash /home/x/R2_Home/R2/tools/install_r2_overnight.sh
```

It will:
- verify `r2`, build tools, `libfaketime`, and the expected deployed files;
- refuse to run beside an already active R2 process;
- check permissions and refuse symlinked database paths;
- create a private snapshot of the memory, Reality, fridge, reward, and addiction databases and require the integrity audit to pass;
- compile the full C runtime to a temporary executable so a failed build does not replace the prior executable;
- install the systemd unit, reload systemd, and enable/start the overnight service.

The installer intentionally stops if the database audit fails. Preserve the report and snapshot and investigate the issue rather than deleting or replacing the database files.

## Verify that R2 is actually active

```sh
systemctl is-enabled r2-3po.service
systemctl is-active r2-3po.service
systemctl --no-pager --full status r2-3po.service
journalctl -u r2-3po.service -n 100 --no-pager
```

Expected state is `enabled` and `active`. The journal should report a successful local Ollama chat preflight and the core readiness marker (`[R2 READY] Core initialized; entering shell.`). The installer does not call the service ready based only on systemd's early `active` state. If the unit keeps restarting, inspect the logs; common causes are a missing `gemma3:4b` model, missing `libfaketime`, inaccessible workspace/database files, or Ollama not running.

Follow live logs with `journalctl -u r2-3po.service -f`.

## Keep the physical computer awake

The runner attempts to acquire a system sleep inhibitor before doing startup checks. If the journal says sleep inhibition is unavailable, disable automatic suspend for the overnight period in the desktop's Power settings, keep the power adapter connected, and do not close the laptop lid unless its action is configured not to suspend. A powered-off or suspended computer cannot continue running R2. The private clock changes only R2's process time; it does not change Linux's system clock.

## Normal operation

- Start: `sudo systemctl start r2-3po.service`
- Stop cleanly: `sudo systemctl stop r2-3po.service`
- Restart: `sudo systemctl restart r2-3po.service`
- Status: `systemctl status r2-3po.service`
- Logs: `journalctl -u r2-3po.service -f`
- Disable automatic boot: `sudo systemctl disable r2-3po.service`

The service intentionally runs without an interactive `R2>` prompt. Stop the service before launching `R2_Launch_Code.sh`; the desktop launcher checks whether the service is active to help prevent concurrent database writers.

After future source changes, stop R2 and rerun the installer so the executable is rebuilt, the database audit is repeated, and the service is restarted safely. The installer does not create or rename the local FX assets.

## Verification boundary

CI can compile the source and statically check the supervisor wiring, but it cannot verify the host's Linux permissions, installed Ollama model, actual SQLite contents, speaker/audio routing, camera access, or desktop power settings. After activation, verify `systemctl is-enabled r2-3po.service` and `systemctl is-active r2-3po.service`, inspect the journal for model/preflight/audio warnings, and test one real conversation with the local model. Separately, launch the interactive desktop session and verify speaker playback, explicit microphone listening, camera/Eyes input, the Observer window, and TV/VCR playback; the overnight systemd service is intentionally headless and is not proof that desktop GUI/audio devices work. Confirm shutdown/restart once and inspect the logs for clean lifecycle messages. The FX collection is a local runtime asset, not a repository folder.
