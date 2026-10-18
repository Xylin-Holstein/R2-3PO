# Manual source audit — rollout candidate

Candidate branch: `release/r2-workable-chat-integration-2026-10-18`
PR: #161 (draft; do not merge while blockers remain)

This is a code-review log, not a CI report. Each finding must be tied to the actual source behavior and marked fixed only after the implementation and its regression coverage are updated. A passing workflow does not close a finding by itself.

## Finding log

### AUDIT-001 — privileged chmod followed symlinks in the runtime-writable tree
- **File:** `tools/install_r2_overnight.sh`
- **Severity:** High
- **Evidence:** The installer used root-level `chmod 0755` on `$RUNNER_SOURCE` and `$OLLAMA_RUNNER_SOURCE`, both under `/home/x/R2_Home/R2`, which must remain writable by `r2`. `chmod` follows a symlink, so a runtime account able to replace one of those paths could cause the privileged operation to change permissions on an unrelated target.
- **Fix:** Change the operation to `run_as_r2 chmod`; add an assertion in `tests/build_configuration_smoke.py` that rejects the old root-level operation.
- **Status:** Fixed in the current candidate head; workflow result for the resulting head must be checked.

### AUDIT-002 — privileged installer and unit-file trust boundary
- **Files:** `tools/install_r2_overnight.sh`, `deploy/r2-3po.service`, `deploy/r2-ollama.service`
- **Severity:** Release blocker
- **Evidence:** The privileged installer itself is stored in the runtime-writable source tree, and it installs systemd unit contents read from that same tree. A compromised `r2` account can modify the privileged entrypoint before it is invoked, or replace the unit files that root installs. Running the build and model copier as `r2` does not fix this boundary.
- **Fix:** Added `tools/r2_install_root_bootstrap.sh`, which requires root, embeds both service-unit definitions, performs the build/model-copy/audit/preflight as `r2`, and installs only the embedded unit files from a root-owned staging directory. Changed `tools/install_r2_overnight.sh` into a nonprivileged notice that refuses root execution. Updated deployment documentation and added assertions that the embedded units match the reviewed `deploy/*.service` files.
- **Status:** Repository-side implementation added; **target setup gate remains open** until the bootstrap is installed as `/usr/local/sbin/r2-3po-install` from the reviewed release and verified root-owned/not group- or world-writable.

### AUDIT-003 — target-host behavior has not been established by source review
- **Areas:** Qwen generation and GPU placement, multi-turn chat, persistence/restart, microphone/camera/audio, TV/VCR, Game Boy, and clean shutdown.
- **Severity:** Release blocker for a claim of rollout readiness
- **Evidence:** GitHub CI cannot exercise the actual GTX 1050 Ti, installed model, local SQLite contents, audio session, camera, or attached devices. No target-host acceptance evidence is available to this audit.
- **Required fix:** After source blockers are resolved, run the documented acceptance checklist on the actual machine, preserve logs, and record each result. Do not ask for another OS update until the repository-side work is consolidated.
- **Status:** Open; requires the target machine.

## Audit scope and rule

The current review is tracing the changed launcher, chat serialization/context allocator, tool-result history, model copier, preflight, service runner, installer, unit files, and the tests that assert those contracts. Findings are added here as concrete source defects are confirmed. “Not found yet” is not equivalent to “proven correct,” and neither syntax checks nor PR status closes a semantic finding.

### AUDIT-004 — failed private-model startup replaced the prior executable too early
- **File:** `tools/install_r2_overnight.sh`
- **Severity:** Medium / rollout reliability
- **Evidence:** The installer previously replaced `$R2_EXEC` before starting the dedicated Ollama service and proving it could generate a real response. If model startup or model preflight failed, the known-good executable had already been displaced and the rollback routine was not invoked on those early exits.
- **Fix:** Keep the freshly built binary at its unique temporary path until the private Ollama service passes the real generation preflight. Only then preserve the old executable and atomically replace it. Add a regression assertion that the real model preflight precedes the backup/replacement sequence.
- **Status:** Fixed in the current candidate head; workflow result for the resulting head must be checked.

### AUDIT-005 — protected bootstrap entrypoint and unit-definition consistency
- **Files:** `tools/r2_install_root_bootstrap.sh`, `tools/install_r2_overnight.sh`, `deploy/r2-3po.service`, `deploy/r2-ollama.service`, `tests/build_configuration_smoke.py`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High
- **Implementation:** The privileged bootstrap refuses to run from the mutable checkout, verifies the installed path is root-owned and not group/world-writable, pins a safe system PATH, embeds the systemd units and writes them from a root-owned staging directory, instead of installing mutable unit files from `R2_SOURCE`. The mutable model copier, database audit, preflight, and build are invoked as `r2`. The source-tree installer is a nonprivileged notice. Regression coverage compares the embedded units byte-for-byte with the repository unit files and asserts the wrapper has no privileged operations.
- **Status:** Implemented; awaiting the exact-head workflow and manual semantic review of the bootstrap path.

### AUDIT-006 — installer preflight did not test the service's actual GPU/context policy
- **Files:** `tools/r2_install_root_bootstrap.sh`, `tools/r2_service_runner.sh`, `tools/preflight_ollama_model.py`, `tests/ollama_preflight_smoke.py`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High / rollout reliability
- **Evidence:** The installer previously called the real generation probe without `R2_OLLAMA_NUM_GPU` or `R2_OLLAMA_NUM_CTX`, so it could pass with automatic placement/default context even though the service's 20-layer/8192-context policy would fail. That left the actual hardware policy to fail only after executable replacement and service activation.
- **Fix:** The trusted bootstrap now detects the target GTX 1050 Ti and probes with 20 GPU layers and 8192 context before replacing the executable; if that default fails, it retries with 12 layers and 4096 context, matching the service runner's fallback. The shared preflight can require a real mixed placement report (positive `size_vram` and total model memory greater than VRAM), and the interactive and overnight launch paths pass that requirement when requesting GPU layers on the target card. Regression coverage checks the policy options and rejects CPU-only success.
- **Status:** Implemented in source; exact-head workflow and target GPU allocation still need verification.

### AUDIT-007 — inhibitor self-exec discarded GPU/context fallback provenance
- **File:** `tools/r2_service_runner.sh`
- **Severity:** High / hardware startup reliability
- **Evidence:** The runner detects that 20 GPU layers and 8192 context were defaults, then re-execs itself under `systemd-inhibit`. The re-exec inherited the resolved values but the old code reset both `*_DEFAULTED` flags to zero because the values were no longer empty. A failed first preflight therefore skipped the intended 12-layer/4096-context fallback.
- **Fix:** Initialize the flags from inherited values and export them across the inhibitor re-exec. Updated `tests/gpu_context_budget_smoke.py` to assert that default provenance is preserved and explicit operator settings remain distinguishable.
- **Status:** Fixed in source; exact-head workflow still pending.

### AUDIT-008 — permission regression test still targeted the retired installer implementation
- **File:** `tests/test_overnight_permission_checks.sh`
- **Severity:** CI regression (not a runtime defect)
- **Evidence:** After the privileged logic moved into the root-owned bootstrap, the test still searched `tools/install_r2_overnight.sh` for the old privileged build/access-check implementation. The source-tree file is now intentionally only a nonprivileged notice, so the stale test failed even though the implementation had moved.
- **Fix:** Repointed the test to `tools/r2_install_root_bootstrap.sh`, assert the protected-path/root-only contract and embedded unit installation, and keep independent checks on the service runner's writable database paths.
- **Status:** Test updated; exact-head workflow still needs to confirm the regression is gone.
### AUDIT-009 — overnight core lacked camera device group access
- **Files:** `deploy/r2-3po.service`, embedded unit in `tools/r2_install_root_bootstrap.sh`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High / vision integration
- **Evidence:** The isolated Ollama service had `video render` supplementary groups, but the R2 core service only had `audio`. The core's camera/Eyes path runs under the core service UID, so device nodes restricted to the `video` group would remain inaccessible even while the model server had GPU access.
- **Fix:** Added `video render` to the R2 core service's supplementary groups, synchronized the embedded trusted unit, and added a regression assertion. The embedded unit consistency check ensures both deployed definitions stay aligned.
- **Status:** Fixed in source; requires confirmation on the target machine that the groups exist and camera devices are accessible.


### AUDIT-010 — installer had unhandled early exits after executable replacement
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** High / rollback reliability
- **Evidence:** The explicit rollback path covered systemd activation/readiness failures, but a failure immediately after the atomic executable replacement (for example, the following chmod or a later command under `set -e`) could exit before reaching that path. The prior executable backup would remain unused while the replacement stayed installed.
- **Fix:** Extended the existing EXIT cleanup trap to restore the previous executable for any nonzero exit after replacement, clean the temporary build, and preserve the existing private-Ollama service-state restoration. The explicit rollback path clears the replacement flag after restoring the prior binary to avoid duplicate rollback. If a first install has no previous binary, the trap stops R2 and removes the failed replacement rather than leaving it runnable.
- **Status:** Implemented; exact-head workflow and review of failure ordering still pending.
### AUDIT-011 — system-override clipping could split a UTF-8 code point
- **File:** `r2.c`
- **Severity:** Medium / prompt reliability
- **Evidence:** When a long system override exceeded its assigned budget, the sentence-boundary search fell back to the midpoint of the byte budget if it found no punctuation. That midpoint was not guaranteed to be a UTF-8 boundary, even though the rest of the serializer uses UTF-8-safe clipping. Passing the resulting byte prefix to json-c could produce invalid UTF-8 and fail or corrupt a request.
- **Fix:** Re-align the chosen override length with `r2_utf8_safe_prefix_length()` after sentence-boundary selection and before constructing the bounded JSON string.
- **Regression coverage:** Extended `tests/prompt_budget_smoke.py` to require that the final UTF-8 alignment occurs after the boundary choice and before serialization.
- **Status:** Source fix and regression assertion committed on the candidate branch. Exact-head workflow and continued semantic review remain open.

### AUDIT-012 — installer could leave the private Ollama service in the wrong state after an early failure
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** Medium / rollout reliability
- **Evidence:** The installer stopped `r2-ollama.service` before copying the model store. Failures in the model copy, build, or real model preflight exited through `fail()` without restoring whether the service had been active/enabled before the attempt. A failed install could therefore strand an existing model service stopped, or leave a previously disabled service enabled after a failed preflight.
- **Fix:** Capture the original active/enabled state before the first service mutation. A single exit cleanup handler now restores that state on unsuccessful exits; if rollback has successfully restarted the previous R2 core, it leaves the model service available to that core. Successful deployment suppresses restoration.
- **Regression coverage:** Added a focused installer lifecycle assertion to `tests/r2_overnight_service_smoke.py`.
- **Status:** Source fix and regression assertion committed on the candidate branch; the exact new head still needs a full manual review and workflow result. This does not close target-host acceptance.



