# Running R2-3PO overnight with systemd

R2 already has a headless shell mode, a supervised systemd unit, a local-model preflight, a private continuously advancing clock, and a sleep-inhibitor attempt. Both launch paths use the fixed `-852076800`-second offset; they must not recompute a 1999 start date on relaunch, because the clock must roll naturally into 2000 and beyond. Use the existing deployment files below; do not create a second service or run the interactive launcher alongside it.

- R2 runner: `tools/r2_service_runner.sh`
- R2 unit: `deploy/r2-3po.service`
- Dedicated model-server runner/unit: `tools/r2_ollama_runner.sh`, `deploy/r2-ollama.service`
- Local model copier: `tools/copy_ollama_model.py`
- Trusted bootstrap source: `tools/r2_install_root_bootstrap.sh` (install once as `/usr/local/sbin/r2-3po-install`); `tools/install_r2_overnight.sh` is only a nonprivileged notice.

The overnight core uses the same persistent R2 databases as the interactive launcher, but its inference server is separate: the installer copies the already-installed `qwen3.5:9b` model from the host Ollama service at `127.0.0.1:11434` into `/home/x/R2_Home/.ollama/models`, then runs an `r2`-owned Ollama server on `127.0.0.1:11435`. It does not download model weights or modify the host Ollama service. **Only one R2 core may run at a time.** Both launch paths keep `HOME` set to `/home/x/R2_Home`, while the process UID remains `r2`; `/home/r2` is not an alternate memory, diary, or workspace location. While the overnight core is active, its stricter UID-scoped firewall chain permits only `127.0.0.1:11435`; IPv6 and all other egress for UID `r2` are rejected. The private Ollama service has its own chain, which also permits local port 11434 so the interactive launcher can use the host Ollama after the overnight core is stopped. The core chain is inserted first while the service is active, so port 11434 is not available to `r2` during overnight operation. The separate chains are cleaned up independently so stopping one service does not remove the other's rules. The installer refuses to rebuild while an R2 process is already running, checks database paths and permissions, takes a private SQLite snapshot, requires a clean integrity audit, copies only an already-installed model, builds to a process-specific temporary executable, preserves the previous binary, and starts the private model server only after installation checks. If the private model preflight fails, R2 is not started; if R2 activation fails, it attempts to restore and restart the previous executable.

## One-time host preparation

Run these on the R2 Linux machine, not on GitHub.

Before installing or starting the service, run the non-destructive hardware preflight from your normal desktop account:

```sh
bash /home/x/R2_Home/R2/tools/preflight_r2_hardware.sh
```

It checks the source/deployment files, the local Ollama API and `qwen3.5:9b`, read-only SQLite integrity, the separate FX folder, and available audio/camera/TV/emulator dependencies. `[FAIL]` items block readiness; `[WARN]` items identify optional hardware or GUI features that need attention. The report does not install packages, change databases, rebuild R2, or start/stop the service. It distinguishes the desktop account's audio session from the `r2` account's default PulseAudio/PipeWire connection and checks whether `r2` can access detected camera and ALSA device nodes when non-interactive `sudo` permits verification. A warning means access was unavailable or could not be proven; it does not itself repair permissions. The interactive launcher adds the host's `audio` group only to the R2 process's supplementary groups when that group exists, preserving the account's existing groups and avoiding a permanent account-membership change. The overnight systemd unit uses the same service-scoped audio group. This allows direct ALSA access where available, but it does not grant access to the desktop user's PulseAudio/PipeWire socket. The final speaker, microphone, and camera test must still be performed from the actual R2 launch environment.

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
   ollama pull qwen3.5:9b
   curl -fsS http://127.0.0.1:11434/api/tags
   ```

   If Ollama is installed without a system unit named `ollama.service`, start it using the host's existing method. The installer copies the already-installed `qwen3.5:9b` blobs into R2's private model store and starts `r2-ollama.service` on port 11435. It then requires both a real text completion and a 640x480 image-bearing JSON-mode completion that identifies a known blue square, with the same context-dependent output cap used by Visual.c, using the selected GPU/context policy (up to 180 seconds per inference request) before starting the R2 core. This catches image-encoder/KV-cache OOMs that a text-only probe can miss. The default GTX 1050 Ti policy starts at 8 GPU layers / 4096 context; if the real text-and-image preflight fails, startup is blocked with the server error instead of retrying after a failed allocation. The copy and inference checks do not download model weights or contact an external API; the host Ollama service on port 11434 is left running and unmodified.

5. Confirm the service account can access the existing workspace and local FX assets. Do not solve permission problems with broad `chmod 777` permissions:

   ```sh
   sudo -u r2 test -x /home/x/R2_Home && echo "workspace traversable"
   sudo -u r2 test -r /home/x/R2_Home/R2/r2.c && echo "source readable"
   sudo -u r2 test -w /home/x/R2_Home/R2 && echo "persistent data directory writable"
   sudo -u r2 test -r /home/x/R2_Home/R2_sounds/FX && echo "FX directory readable"
   ```

   If a check fails, correct the existing owner/group/ACL deliberately before continuing. The core stores its five SQLite databases under `R2/` (`r2_memory.db`, `r2_reality.db`, `r2_fridge.db`, `r2_rewards.db`, and `r2_addictions.db`), so account `r2` needs read/write access to that directory and its existing database files.

## Local inference timeout

Foreground chat and its corrective retry allow up to six hours per request by default to accommodate slow mixed CPU/GPU inference and Qwen's reasoning channel. Override the setting before starting the interactive launcher if needed:

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

Accepted values are 30–21,600 seconds (30 seconds to six hours); invalid values use the six-hour default. This only changes how long R2 waits for a request, not inference speed.

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

### Install the trusted root bootstrap once

Do **not** run a source-tree script with `sudo`. The checkout under `/home/x/R2_Home/R2` must remain writable by `r2` for the existing persistent SQLite databases and runtime files. A privileged installer stored there would let a compromised runtime account replace code that is later executed as root.

Do **not** copy the bootstrap directly from `/home/x/R2_Home/R2`: that tree is writable by `r2`, so checking it and then running `sudo install` from the same path leaves a time-of-check/time-of-use gap. Fetch the reviewed bootstrap from an immutable commit into the desktop user's home (outside the R2-writable tree), verify its Git blob ID, then install that exact file:

```sh
BOOTSTRAP_COMMIT=2baef2066672c7447ee4163e9d8f0807c922bace
BOOTSTRAP_BLOB_SHA=f6d1ce4a55823df1f30118f6f89397caba193dbf
umask 077
curl -fsSL \
  "https://raw.githubusercontent.com/Xylin48/R2-3PO/${BOOTSTRAP_COMMIT}/tools/r2_install_root_bootstrap.sh" \
  -o /home/x/r2-3po-install
chmod 0600 /home/x/r2-3po-install
ACTUAL_BLOB_SHA="$(git hash-object /home/x/r2-3po-install)"
if [[ "$ACTUAL_BLOB_SHA" != "$BOOTSTRAP_BLOB_SHA" ]]; then
  rm -f /home/x/r2-3po-install
  echo "ERROR: trusted bootstrap hash mismatch; refusing installation." >&2
  exit 1
fi
sudo install -o root -g root -m 0755 \
  /home/x/r2-3po-install /usr/local/sbin/r2-3po-install || {
    rm -f /home/x/r2-3po-install
    echo "ERROR: failed to install the trusted root bootstrap." >&2
    exit 1
  }
if ! sudo test -f /usr/local/sbin/r2-3po-install || \
   ! sudo test ! -L /usr/local/sbin/r2-3po-install || \
   [[ "$(sudo stat -c '%u:%a' /usr/local/sbin/r2-3po-install)" != "0:755" ]]; then
  echo "ERROR: installed bootstrap is not a root-owned, non-symlink 0755 file." >&2
  exit 1
fi
rm -f /home/x/r2-3po-install
```

The commit URL is immutable, and the Git blob hash must match exactly. Do not replace this with a copy from the runtime-writable checkout. If the bootstrap source changes in a future release, review the new file and deliberately update both the pinned commit and blob hash.

From then on, use only the protected entrypoint:

```sh
sudo /usr/local/sbin/r2-3po-install
```

The trusted bootstrap compiles the current checkout and runs the model copier, database audit, and preflight as the unprivileged `r2` account. Its systemd unit definitions are embedded in the root-owned bootstrap; it does not install unit contents read from the runtime-writable checkout. The repository's `tools/install_r2_overnight.sh` is now only a nonprivileged notice and must not be invoked with `sudo`. If the trusted bootstrap itself is intentionally changed in a future release, review that change and reinstall the root-owned copy deliberately.

The bootstrap will:
- verify `r2`, build tools, `libfaketime`, and the required runtime files;
- refuse to run beside an already active R2 process;
- check permissions and refuse symlinked database paths;
- create a private snapshot of the memory, Reality, fridge, reward, and addiction databases and require the integrity audit to pass;
- compile the full C runtime as `r2` and keep the previous executable until the private Ollama server passes a real generation preflight;
- install the trusted embedded systemd units, reload systemd, and enable/start the overnight service.

The bootstrap intentionally stops if the database audit or model preflight fails. Preserve the report and snapshot and investigate the issue rather than deleting or replacing the database files.

## Verify that R2 is actually active

```sh
systemctl is-enabled r2-3po.service
systemctl is-active r2-3po.service
systemctl --no-pager --full status r2-3po.service
journalctl -u r2-3po.service -n 100 --no-pager
```

Expected state is `enabled` and `active`. The journal should report successful text and synthetic-image Ollama preflights and the core readiness marker (`[R2 READY] Core initialized; entering shell.`). The installer does not call the service ready based only on systemd's early `active` state. If the unit keeps restarting, inspect the logs; common causes are a missing `qwen3.5:9b` model, missing `libfaketime`, inaccessible workspace/database files, or Ollama not running.

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


### CPU/GPU placement policy

On the target 4 GiB GTX 1050 Ti, the interactive launcher, overnight runner, and trusted installer default to 8 GPU layers and a 4096-token context. This avoids the previously observed CUDA allocation failure from the larger 20-layer / 8192-context first probe. Before replacing the executable or entering the core, preflight must prove a real text completion, a real image-bearing completion, **and** mixed placement from Ollama's `/api/ps` report: positive `size_vram` and total model size greater than VRAM. If that preflight fails, startup remains blocked and reports the server error; there is no larger default probe or retry after the failed allocation. The core adapts context further to available RAM.

On a host with no NVIDIA PCI device, `R2_OLLAMA_NUM_GPU=0` omits an explicit `num_gpu` option and lets Ollama choose an available backend automatically. If an NVIDIA PCI device is present but `nvidia-smi` cannot query it, startup fails rather than silently downgrading to CPU-only inference. A successfully queried non-1050-Ti NVIDIA GPU uses automatic layer placement but must still prove mixed placement. Explicit operator overrides are respected rather than silently changed; `R2_OLLAMA_NUM_GPU=0` means automatic placement, not permission to ignore an installed NVIDIA GPU. `-1` is an explicit all-layers request, not a safe universal default, and it is incompatible with the target's required mixed-placement check if Ollama reports the whole model in VRAM.
