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

### AUDIT-002 — privileged installer and unit-file trust boundary remains open
- **Files:** `tools/install_r2_overnight.sh`, `deploy/r2-3po.service`, `deploy/r2-ollama.service`
- **Severity:** Release blocker
- **Evidence:** The privileged installer itself is stored in the runtime-writable source tree, and it installs systemd unit contents read from that same tree. A compromised `r2` account can modify the privileged entrypoint before it is invoked, or replace the unit files that root installs. Running the build and model copier as `r2` does not fix this boundary.
- **Required fix:** Move privileged deployment logic to a trusted root-owned bootstrap that does not execute or install privileged configuration from the runtime-writable tree. The current layout can keep databases writable, but the root bootstrap and service-unit contents must come from a trusted protected location. Preserve the existing database files and never solve this with broad permissions.
- **Status:** Open; candidate is not installer-trust-hardened.

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
