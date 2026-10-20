# R2 rollout-readiness gate

This checklist separates repository/CI evidence from checks that must run on the target Linux host. Passing GitHub Actions is necessary, but it is not proof that the robot's model, live databases, microphone, camera, speaker, TV, or attached devices work.

## 1. Confirm the candidate and CI

- Consolidated integration PR: [#167](https://github.com/Xylin48/R2-3PO/pull/167), branch `integrate/r2-hardware-ready-2026-10-20`. This branch is cut from the integrated completion candidate plus the rollout checklist update, and includes all 146 relevant changed files in the complete runtime/test/tool/docs integration.
- Runtime candidate PR [#165](https://github.com/Xylin48/R2-3PO/pull/165) head commit `69214901e1d4c62ac036aecb85d44b6c6663ce4d` passed exact-head workflow [#5478](https://github.com/Xylin48/R2-3PO/actions/runs/39402852794). The code-bearing integration commit `770e3e3756af90c78e0d5a821ab31a0b4e847806` passed the complete `R2 build` workflow [#5492](https://github.com/Xylin48/R2-3PO/actions/runs/39474767159), including the generic-clarification retry regression, strict production compile, seasonal-shell checks, and deployment lifecycle tests. The subsequent audit/readiness documentation refresh commit `e726b8c0b4f8cdf61dba65d743a930cb400705e5` also passed workflow [#5495](https://github.com/Xylin48/R2-3PO/actions/runs/39475446534). CI remains distinct from target-host acceptance.
- The integration carries conversation retry/history fixes, contextual (not scripted) affect interpretation, seasonal shell themes, subsystem/database compatibility, speech/vision wiring, Game Boy/TV, launcher/service lifecycle, and regression coverage. Do not replace newer source files with older branch versions.
- Do not merge to `main` or call the candidate hardware-verified based on CI alone. Target-host acceptance remains required for live Qwen 3.5 9B multi-turn behavior, measured GTX 1050 Ti mixed CPU/GPU placement and RAM headroom, real database writes/restart persistence, and attached audio/video/device paths.

## 2. Preflight the target host before activation

Run these on the target Linux machine, not in CI:

```sh
bash /home/x/R2_Home/R2/tools/preflight_r2_hardware.sh
```

Resolve every `[FAIL]`. Review each `[WARN]` and confirm whether the feature is optional or required for this installation. The preflight is intended to be non-destructive; it does not install packages, migrate databases, or start/stop R2.

Interactive R2 uses the desktop Ollama endpoint on `127.0.0.1:11434`; the isolated overnight runner uses `127.0.0.1:11435`. Both share the GTX 1050 Ti's VRAM, so a loaded runner on one endpoint can cause the other endpoint's inference to fail while its own `/api/ps` appears empty. The interactive launcher now pauses `r2-ollama.service` when it is active and restores it on every exit path; it refuses to start a second R2 core if `r2-3po.service` is active. If a `llama-server` remains on the GPU while the isolated service is inactive, inspect `systemctl status ollama.service r2-ollama.service` and identify the owning process before stopping anything; do not kill an unrelated runner blindly.

For a read-only database inventory and integrity report:

```sh
python3 /home/x/R2_Home/R2/tools/audit_r2_databases.py \
  --root /home/x/R2_Home/R2
```

For non-overwriting snapshots, choose a new destination directory for each run. For a cross-database-consistent set, first exit the interactive launcher cleanly if it is running. If R2 is running as the overnight service, stop that service before continuing. Do not proceed while either R2 core is active.

```sh
# Run this line only when the overnight systemd service is the active runtime:
sudo systemctl stop r2-3po.service
python3 /home/x/R2_Home/R2/tools/audit_r2_databases.py \
  --root /home/x/R2_Home/R2 \
  --snapshot-dir "/home/x/R2_Home/R2_Database_Snapshots/rollout-$(date +%Y%m%d-%H%M%S)"
```

Do not delete or replace legacy databases to make a check pass. Investigate integrity failures or unexpected schema/row reports first. The audit tool is read-only against source databases; snapshots are new files.

## 3. Install and verify one runtime mode at a time

Follow [the overnight service guide](overnight-service.md) for dependency preparation and the installer. The service copies the already-installed `qwen3.5:9b` model into R2's private local Ollama store and requires a real local chat completion before starting the core.

The model-copy step validates the Ollama model reference and manifest structure before writing, rejects symlinked source manifests/blob files and source/destination parent components, checks every blob digest and source blob hash, verifies copied content, and uses exclusively-created temporary files for atomic replacement. The model preflight then runs both a real text completion and a camera-sized 640x480 synthetic-image completion in JSON mode that must identify a known blue square, using the same context-dependent output cap as Visual.c; image-bearing inference failure/OOM blocks startup and reports the Ollama error; the launcher does not retry after an oversized allocation. The shared preflight also reports Linux `MemAvailable` after both generations complete, while the model is still kept warm, so RAM headroom can be recorded alongside `/api/ps` GPU-placement evidence. Aim for roughly 3.8 GiB of available host RAM where feasible; treat that as a measured target rather than a hard-coded startup gate, since desktop services and device workloads affect the remaining memory.

On the 4 GiB GTX 1050 Ti, the interactive launcher, overnight runner, and trusted installer now begin with the conservative `R2_OLLAMA_NUM_GPU=12` / `R2_OLLAMA_NUM_CTX=4096` policy. The previous 20-layer / 8192-context first probe caused a CUDA allocation failure, and retrying after that failed allocation needlessly repeats an avoidable failure against the same local model service. Explicit interactive/service environment overrides remain respected where supported. A failed real text-and-image preflight blocks startup and preserves the previous executable; there is no larger default probe or same-policy retry. On the GTX 1050 Ti path, a generation reply alone is insufficient: preflight must report positive VRAM allocation and total model memory greater than VRAM to confirm mixed CPU/GPU placement. It must reject malformed references/manifests and must not follow pre-planted destination or temporary-file symlinks. These checks protect the copy operation; they do not replace the target-host model-generation and GPU-placement checks below.

**Trusted bootstrap requirement:** the repository now includes `tools/r2_install_root_bootstrap.sh`, which is intended to be installed once as the root-owned `/usr/local/sbin/r2-3po-install`. It contains the systemd unit definitions and does not execute or install privileged unit content from `R2_SOURCE`; the mutable model copier, database audit, preflight, and build all run as `r2`. `tools/install_r2_overnight.sh` is only a nonprivileged notice and must not be run with `sudo`. The bootstrap must be fetched from immutable commit `da339ce2ed6b8a7802fe846edf500ff27cbe26d0` into `/home/x` (outside the runtime-writable tree), verified against Git blob SHA `93d9c04d836cbe567109470404f423213260dc19`, and only then installed as `/usr/local/sbin/r2-3po-install`. Never install it directly from `/home/x/R2_Home/R2`, where `r2` can change it between review and privileged copy. The trust boundary is addressed only when the verified root-owned bootstrap has actually been installed and checked for root ownership and non-writable group/world permissions. Do not run the mutable source-tree installer as root.

**Never run the interactive launcher and the overnight systemd core simultaneously.** They share persistent databases and are designed for one active R2 core.

After installing/activating the service, verify its real state and logs:

```sh
sudo systemctl is-enabled r2-3po.service
sudo systemctl is-active r2-3po.service
sudo journalctl -u r2-3po.service -n 100 --no-pager
```

A successful service status is only the beginning: confirm the logs show a successful mixed CPU/GPU model preflight and no database, permission, or shutdown errors.

## 4. Run real-device acceptance tests

With the interactive desktop launcher (and not the systemd core) verify:

1. Type a multi-turn conversation and confirm the answer responds to the latest user turn rather than retrieved memory or diagnostics.
2. Explicitly invoke microphone listening; confirm the transcript enters the same ordinary conversation path as typed text and errors do not hang shutdown.
3. Exercise camera/Eyes and Watch start/stop, including stopping while vision inference is in progress.
4. Verify speaker/FX playback, Observer behavior, and TV/VCR controls against the actual attached hardware.
5. If deployed, verify Game Boy launch/control and that saves/state are written under the configured device root.
6. Restart R2 and confirm memories, Life Log links, diary, rewards, fridge, and other databases remain readable and writable.
7. Stop R2 cleanly, start it again, and inspect logs for orphaned workers, child processes, silent turns, or repeated model failures.

Use the real installed model and actual hardware; do not substitute mock results for this gate. Record the result of each check before calling the candidate rollout-ready.

## 5. Rollback posture

- Preserve the last known-good executable and the database snapshots created before rollout.
- Do not run schema downgrade scripts or restore a database over a live process.
- If model preflight, integrity checks, or conversation tests fail, stop activation, capture logs, and investigate before retrying.
- Keep changes on this release branch until repository checks and target-host acceptance both pass.
