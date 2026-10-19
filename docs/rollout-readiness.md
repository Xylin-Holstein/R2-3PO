# R2 rollout-readiness gate

This checklist separates repository/CI evidence from checks that must run on the target Linux host. Passing GitHub Actions is necessary, but it is not proof that the robot's model, live databases, microphone, camera, speaker, TV, or attached devices work.

## 1. Confirm the candidate and CI

- Use `release/r2-workable-chat-integration-2026-10-18` as the current consolidated candidate (draft PR #161), based on `release/r2-rollout-readiness-2026-10-17` plus the latest conversation, prompt-budget, and Ollama model-copy integrity fixes. It carries the integrated conversation, speech, vision, database compatibility, Game Boy, TV, and lifecycle work.
- Require the full `R2 build` GitHub Actions workflow to pass on the exact candidate commit. The release branch is explicitly included in the workflow's push filters.
- Do not merge to `main` or call the candidate hardware-verified based on static smoke tests alone.

## 2. Preflight the target host before activation

Run these on the target Linux machine, not in CI:

```sh
bash /home/x/R2_Home/R2/tools/preflight_r2_hardware.sh
```

Resolve every `[FAIL]`. Review each `[WARN]` and confirm whether the feature is optional or required for this installation. The preflight is intended to be non-destructive; it does not install packages, migrate databases, or start/stop R2.

For a read-only database inventory and integrity report:

```sh
python3 /home/x/R2_Home/R2/tools/audit_r2_databases.py \
  --root /home/x/R2_Home
```

For non-overwriting snapshots, choose a new destination directory for each run. For a cross-database-consistent set, first exit the interactive launcher cleanly if it is running. If R2 is running as the overnight service, stop that service before continuing. Do not proceed while either R2 core is active.

```sh
# Run this line only when the overnight systemd service is the active runtime:
sudo systemctl stop r2-3po.service
python3 /home/x/R2_Home/R2/tools/audit_r2_databases.py \
  --root /home/x/R2_Home \
  --snapshot-dir "/home/x/R2_Home/R2_Database_Snapshots/rollout-$(date +%Y%m%d-%H%M%S)"
```

Do not delete or replace legacy databases to make a check pass. Investigate integrity failures or unexpected schema/row reports first. The audit tool is read-only against source databases; snapshots are new files.

## 3. Install and verify one runtime mode at a time

Follow [the overnight service guide](overnight-service.md) for dependency preparation and the installer. The service copies the already-installed `qwen3.5:9b` model into R2's private local Ollama store and requires a real local chat completion before starting the core.

The model-copy step validates the Ollama model reference and manifest structure before writing, rejects symlinked source manifests/blob files and source/destination parent components, checks every blob digest and source blob hash, verifies copied content, and uses exclusively-created temporary files for atomic replacement. The model preflight then runs both a real text completion and a camera-sized 640x480 synthetic-image completion in JSON mode that must identify a known blue square, using the same context-dependent output cap as Visual.c; image-bearing inference failure/OOM blocks startup and triggers the same conservative default-only fallback as text failure.

On the 4 GiB GTX 1050 Ti, both the interactive launcher and overnight runner first probe the default 20-GPU-layer / 8192-context policy. If real generation fails and both settings were defaults—not explicit operator overrides—they retry once at 12 GPU layers / 4096 context to reduce VRAM/KV-cache pressure. A successful fallback becomes the policy used by the core; if it also fails, startup remains blocked and the failure is reported. On the GTX 1050 Ti path, a generation reply alone is insufficient: preflight must report positive VRAM allocation and total model memory greater than VRAM to confirm mixed CPU/GPU placement. It must reject malformed references/manifests and must not follow pre-planted destination or temporary-file symlinks. These checks protect the copy operation; they do not replace the target-host model-generation and GPU-placement checks below.

**Trusted bootstrap requirement:** the repository now includes `tools/r2_install_root_bootstrap.sh`, which is intended to be installed once as the root-owned `/usr/local/sbin/r2-3po-install`. It contains the systemd unit definitions and does not execute or install privileged unit content from `R2_SOURCE`; the mutable model copier, database audit, preflight, and build all run as `r2`. `tools/install_r2_overnight.sh` is only a nonprivileged notice and must not be run with `sudo`. The bootstrap must be fetched from immutable commit `2aec6cee84ed1274a6c7324ec6ac5560d8088240` into `/home/x` (outside the runtime-writable tree), verified against Git blob SHA `6e9906e6ac3e7299b368585fc7e842f32ceb2646`, and only then installed as `/usr/local/sbin/r2-3po-install`. Never install it directly from `/home/x/R2_Home/R2`, where `r2` can change it between review and privileged copy. The trust boundary is addressed only when the verified root-owned bootstrap has actually been installed and checked for root ownership and non-writable group/world permissions. Do not run the mutable source-tree installer as root.

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
