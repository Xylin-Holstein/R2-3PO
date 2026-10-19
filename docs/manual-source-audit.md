# Manual source audit — rollout candidate

Candidate branch: `integrate/affect-and-mother-chat-root-fix-2026-10-19`
PR: #162 (draft; do not merge while rollout blockers remain)

This is a code-review log, not a CI report. Each finding must be tied to the actual source behavior and marked fixed only after the implementation and its regression coverage are updated. A passing workflow does not close a finding by itself.

## Finding log

### AUDIT-001 — privileged chmod followed symlinks in the runtime-writable tree
- **File:** `tools/install_r2_overnight.sh`
- **Severity:** High
- **Evidence:** The installer used root-level `chmod 0755` on `$RUNNER_SOURCE` and `$OLLAMA_RUNNER_SOURCE`, both under `/home/x/R2_Home/R2`, which must remain writable by `r2`. `chmod` follows a symlink, so a runtime account able to replace one of those paths could cause the privileged operation to change permissions on an unrelated target.
- **Fix:** Change the operation to `run_as_r2 chmod`; add an assertion in `tests/build_configuration_smoke.py` that rejects the old root-level operation.
- **Status:** Source fix implemented and regression assertion added; verification is tracked in the snapshot below.

### AUDIT-002 — privileged installer and unit-file trust boundary
- **Files:** `tools/install_r2_overnight.sh`, `deploy/r2-3po.service`, `deploy/r2-ollama.service`
- **Severity:** Release blocker
- **Evidence:** The privileged installer itself is stored in the runtime-writable source tree, and it installs systemd unit contents read from that same tree. A compromised `r2` account can modify the privileged entrypoint before it is invoked, or replace the unit files that root installs. Running the build and model copier as `r2` does not fix this boundary.
- **Fix:** Added `tools/r2_install_root_bootstrap.sh`, which requires root, embeds both service-unit definitions, performs the build/model-copy/audit/preflight as `r2`, and installs only the embedded unit files from a root-owned staging directory. Changed `tools/install_r2_overnight.sh` into a nonprivileged notice that refuses root execution. The one-time install instructions now fetch the bootstrap from an immutable commit into `/home/x`, verify its Git blob SHA, and only then copy it to the protected root path. The source-tree wrapper points to this procedure and no longer prints a privileged copy command from the runtime-writable checkout. Regression assertions keep the embedded units aligned with the reviewed `deploy/*.service` files.
- **Status:** Repository-side implementation and safe install procedure are in place; **target setup gate remains open** until the verified bootstrap is actually installed at `/usr/local/sbin/r2-3po-install` and checked root-owned/not group- or world-writable.

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
- **Status:** Protected entrypoint and embedded-unit consistency implemented and manually reviewed; actual root-owned installation remains a target setup gate.

### AUDIT-006 — installer preflight did not test the service's actual GPU/context policy
- **Files:** `tools/r2_install_root_bootstrap.sh`, `tools/r2_service_runner.sh`, `tools/preflight_ollama_model.py`, `tests/ollama_preflight_smoke.py`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High / rollout reliability
- **Evidence:** The installer previously called the real generation probe without `R2_OLLAMA_NUM_GPU` or `R2_OLLAMA_NUM_CTX`, so it could pass with automatic placement/default context even though the service's 20-layer/8192-context policy would fail. That left the actual hardware policy to fail only after executable replacement and service activation.
- **Fix:** The trusted bootstrap now detects the target GTX 1050 Ti and probes with 20 GPU layers and 8192 context before replacing the executable; if that default fails, it retries with 12 layers and 4096 context, matching the service runner's fallback. The shared preflight can require a real mixed placement report (positive `size_vram` and total model memory greater than VRAM), and the interactive and overnight launch paths pass that requirement when requesting GPU layers on the target card. Regression coverage checks the policy options and rejects CPU-only success.
- **Status:** Source fix and regression coverage implemented. Target GPU allocation and fallback behavior still require the actual machine.

### AUDIT-007 — inhibitor self-exec discarded GPU/context fallback provenance
- **File:** `tools/r2_service_runner.sh`
- **Severity:** High / hardware startup reliability
- **Evidence:** The runner detects that 20 GPU layers and 8192 context were defaults, then re-execs itself under `systemd-inhibit`. The re-exec inherited the resolved values but the old code reset both `*_DEFAULTED` flags to zero because the values were no longer empty. A failed first preflight therefore skipped the intended 12-layer/4096-context fallback.
- **Fix:** Initialize the flags from inherited values and export them across the inhibitor re-exec. Updated `tests/gpu_context_budget_smoke.py` to assert that default provenance is preserved and explicit operator settings remain distinguishable.
- **Status:** Source fix and regression coverage implemented; target fallback behavior remains unverified.

### AUDIT-008 — permission regression test still targeted the retired installer implementation
- **File:** `tests/test_overnight_permission_checks.sh`
- **Severity:** CI regression (not a runtime defect)
- **Evidence:** After the privileged logic moved into the root-owned bootstrap, the test still searched `tools/install_r2_overnight.sh` for the old privileged build/access-check implementation. The source-tree file is now intentionally only a nonprivileged notice, so the stale test failed even though the implementation had moved.
- **Fix:** Repointed the test to `tools/r2_install_root_bootstrap.sh`, assert the protected-path/root-only contract and embedded unit installation, and keep independent checks on the service runner's writable database paths.
- **Status:** Test updated to the trusted-bootstrap contract; workflow verification is tracked in the snapshot below.
### AUDIT-009 — overnight core lacked camera device group access
- **Files:** `R2_Launch_Code.sh`, `deploy/r2-3po.service`, embedded unit in `tools/r2_install_root_bootstrap.sh`, `tests/build_configuration_smoke.py`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High / vision integration
- **Evidence:** The isolated Ollama service had `video render` supplementary groups, but the R2 core service only had `audio`. The core's camera/Eyes path runs under the core service UID, so device nodes restricted to the `video` group would remain inaccessible even while the model server had GPU access.
- **Fix:** Added `video` to the interactive launcher's per-process supplementary groups and added `video render` to the overnight core service unit; synchronized the embedded trusted unit and added regression assertions. The embedded unit consistency check ensures both deployed definitions stay aligned.
- **Status:** Source fix and unit consistency implemented; target group/device access remains to be confirmed.


### AUDIT-010 — installer had unhandled early exits after executable replacement
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** High / rollback reliability
- **Evidence:** The explicit rollback path covered systemd activation/readiness failures, but a failure immediately after the atomic executable replacement (for example, the following chmod or a later command under `set -e`) could exit before reaching that path. The prior executable backup would remain unused while the replacement stayed installed.
- **Fix:** Extended the existing EXIT cleanup trap to restore the previous executable for any nonzero exit after replacement, clean the temporary build, and preserve the existing private-Ollama service-state restoration. The explicit rollback path clears the replacement flag after restoring the prior binary to avoid duplicate rollback. If a first install has no previous binary, the trap stops and disables the failed R2 service, then removes the failed replacement rather than leaving it runnable.
- **Status:** Source fix implemented and manually reviewed; target failure-injection remains unperformed.
### AUDIT-011 — system-override clipping could split a UTF-8 code point
- **File:** `r2.c`
- **Severity:** Medium / prompt reliability
- **Evidence:** When a long system override exceeded its assigned budget, the sentence-boundary search fell back to the midpoint of the byte budget if it found no punctuation. That midpoint was not guaranteed to be a UTF-8 boundary, even though the rest of the serializer uses UTF-8-safe clipping. Passing the resulting byte prefix to json-c could produce invalid UTF-8 and fail or corrupt a request.
- **Fix:** Re-align the chosen override length with `r2_utf8_safe_prefix_length()` after sentence-boundary selection and before constructing the bounded JSON string.
- **Regression coverage:** Extended `tests/prompt_budget_smoke.py` to require that the final UTF-8 alignment occurs after the boundary choice and before serialization.
- **Status:** Source fix and regression assertion implemented; workflow verification is tracked below.

### AUDIT-012 — installer could leave the private Ollama service in the wrong state after an early failure
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** Medium / rollout reliability
- **Evidence:** The installer stopped `r2-ollama.service` before copying the model store. Failures in the model copy, build, or real model preflight exited through `fail()` without restoring whether the service had been active/enabled before the attempt. A failed install could therefore strand an existing model service stopped, or leave a previously disabled service enabled after a failed preflight.
- **Fix:** Capture the original active/enabled state before the first service mutation. A single exit cleanup handler now restores that state on unsuccessful exits; if rollback has successfully restarted the previous R2 core, it leaves the model service available to that core. Successful deployment suppresses restoration.
- **Regression coverage:** Added a focused installer lifecycle assertion to `tests/r2_overnight_service_smoke.py`.
- **Status:** Source fix and regression assertion implemented; lifecycle ordering was manually reviewed. This does not close target-host acceptance.




### AUDIT-013 — rollout documentation disagreed with effective GPU and firewall policies
- **File:** `docs/overnight-service.md`
- **Severity:** Medium / operator correctness
- **Evidence:** The documentation still described automatic GPU placement as the default even though both launch paths request 20 layers/8192 context on the target GTX 1050 Ti. It also implied both local Ollama ports were available to all `r2` processes while the overnight core's stricter firewall chain is inserted first and only permits port 11435.
- **Fix:** Updated the documented target GPU/context policy, strict mixed-placement requirement, one-time fallback, and effective per-service firewall behavior.
- **Status:** Documentation corrected; workflow verification is tracked in the snapshot below.

## File-by-file manual review ledger

| File | Manual review performed | Remaining limitation |
|---|---|---|
| `r2.c` | Traced authoritative current-turn selection, serialized request construction, UTF-8-safe clipping, system-context priority/budgeting, response-channel parsing, visible-answer recovery, tool-result reinsertion, and foreground/background cancellation. | No target-model conversation run is available from this code-review environment. |
| `R2_Launch_Code.sh` | Traced build/preflight order, GPU/context default and fallback, per-process audio/video groups, network-lock installation and cleanup, and nonzero-exit cleanup. | Camera/microphone/audio device access needs one target-host acceptance pass. |
| `tools/r2_service_runner.sh` | Traced runtime permission checks, private endpoint pinning, systemd-inhibit self-exec, preservation of default provenance, mixed-placement preflight, fallback, and service startup. | The GTX 1050 Ti's actual VRAM/RAM allocation is host-only evidence. |
| `tools/r2_install_root_bootstrap.sh` | Traced root-only protected-path guard, fixed PATH, mutable operations dropped to `r2`, DB snapshot/audit, model copy, exact embedded unit content, preflight-before-replacement, rollback, and private-Ollama state restoration. | The protected bootstrap must be installed and verified root-owned on the target; failure-injection has not been run there. |
| `tools/preflight_ollama_model.py` | Traced endpoint allowlist, installed-model check, real text generation, generated known-color PNG vision request, response validation, context/GPU options, and required mixed-placement report. | Must observe the actual target API's `size`/`size_vram` values. |
| `tools/copy_ollama_model.py` | Reviewed source-store discovery, manifest/layer digest validation, path/symlink rejection, verified atomic blob/manifest copy, and ownership handling. | Actual host model-store permissions and manifest must be confirmed by preflight. |
| `tools/install_r2_overnight.sh` | Confirmed it is now a nonprivileged notice and refuses root execution; it no longer acts as the privileged deployment entrypoint. | None in its intended role. |
| `deploy/r2-3po.service` and `deploy/r2-ollama.service` | Reviewed account/device groups, sandbox paths, endpoint environment, firewall-chain setup/cleanup, restart/dependency behavior, and the effective stricter core egress policy. Embedded copies were compared byte-for-byte with the repository units. | Actual group existence, device permissions, and firewall behavior need host acceptance. |
| `tests/build_configuration_smoke.py`, `tests/gpu_context_budget_smoke.py`, `tests/ollama_preflight_smoke.py`, `tests/nonresponsive_reply_retry_smoke.py`, `tests/r2_reply_misclassification_smoke.c`, `tests/r2_overnight_service_smoke.py`, `tests/test_overnight_permission_checks.sh`, and prompt/context regression tests | Reviewed assertions against the actual implementation; added regression coverage for the pinned bootstrap blob, camera-sized JSON vision inference, short-turn misclassification recovery, GPU placement, fallback provenance, embedded units, camera groups, and early rollback. | Tests cannot substitute for hardware acceptance. |
| `docs/overnight-service.md`, `docs/rollout-readiness.md`, `docs/conversation-quality-regression.md` | Reviewed installation sequence, root trust boundary, effective firewall behavior, GPU/context policy, and acceptance checklist for consistency with source. | The acceptance checklist must be completed on the target machine. |

## Verification snapshot

The full 86-step GitHub Actions workflow passed for candidate code head `5bb6ff4f2e2b6b95b04dcd5bc2285d0c75941b42` (run `39121135543`). This run includes the short-turn recovery regression, the 640x480 JSON-mode vision preflight, the pinned-bootstrap hash assertion, and the deployment/lifecycle tests. This is recorded separately from manual source review: neither a passing workflow nor this table establishes that the physical machine has passed acceptance.


### AUDIT-014 — broken NVIDIA runtime detection could silently select CPU-only inference
- **Files:** `R2_Launch_Code.sh`, `tools/r2_service_runner.sh`, `tools/r2_install_root_bootstrap.sh`, `tools/preflight_r2_hardware.sh`, `tests/build_configuration_smoke.py`
- **Severity:** High / target hardware correctness
- **Evidence:** GPU layer defaults and the strict mixed-placement requirement were previously enabled only when `nvidia-smi` successfully named the GTX 1050 Ti. If the card remained on the PCI bus but the driver/runtime query failed, the launch paths could fall back to automatic/CPU placement and pass a text/vision generation without proving GPU use.
- **Fix:** Detect NVIDIA PCI vendor IDs and display-class codes independently. If an NVIDIA display device is present but `nvidia-smi` cannot query a GPU, the interactive launcher, service runner, installer, and read-only hardware preflight now fail closed. Any detected NVIDIA display device also requires the mixed-placement report; the target GTX 1050 Ti gets the 20-layer/8192-context default and 12-layer/4096-context fallback. Automatic layer placement remains valid only if Ollama's process report proves mixed use.
- **Status:** Source fix and regression assertions passed in the full workflow at code head `5bb6ff4f2e2b6b95b04dcd5bc2285d0c75941b42`; target hardware must still demonstrate the actual driver query and mixed-placement values.


### AUDIT-015 — text-only preflight did not exercise the multimodal path
- **File:** `tools/preflight_ollama_model.py`
- **Severity:** High / target GPU readiness
- **Evidence:** The installer and both launch paths used a successful text-only chat request as their model-readiness signal. That did not exercise Qwen's image encoder or the image-bearing request path, despite the target GTX 1050 Ti having already shown multimodal CUDA OOM risk. A text probe could therefore pass, activate R2, and leave Eyes/Watch vision failing at runtime.
- **Fix:** Preflight now sends a camera-sized 640x480 PNG with a known blue square through `/api/chat` in JSON mode, uses the same GPU/context policy and context-dependent output cap as `Visual.c`, then parses the JSON and requires `color=blue`. An image request failure or incorrect result blocks readiness, allowing the existing default-only 20/8192 → 12/4096 fallback to run.
- **Regression coverage:** `tests/ollama_preflight_smoke.py` verifies PNG construction, policy propagation to the image request, endpoint selection, known-color response checking, and rejection of simulated CUDA OOM.
- **Status:** Source fix and focused regression tests passed in the full workflow at code head `5bb6ff4f2e2b6b95b04dcd5bc2285d0c75941b42`. Real camera inference still belongs to target-host acceptance.

### AUDIT-016 — explicit reply-misclassification phrases were skipped on short turns
- **File:** `r2.c`
- **Severity:** Medium / conversation correctness
- **Evidence:** `r2_reply_misclassifies_plain_text()` returned early for queries shorter than 24 bytes or fewer than five words before checking known failure phrases. A short, meaningful turn such as `Hi` could therefore receive a reply calling it a “random assortment of sounds” without triggering the retry path, even though the response phrase was an explicit misclassification signal.
- **Fix:** Check the existing known-failure phrase set before the minimum-length and word-count guards, while retaining the audio/transcription-topic exemption and the separate acknowledgment path for user feedback about R2's own output.
- **Regression coverage:** Added a short-turn test that requires the explicit misclassification phrase to trigger recovery; existing short-conversation and audio/transcription tests remain in place.
- **Status:** Source fix and focused regression test passed in the full workflow at code head `5bb6ff4f2e2b6b95b04dcd5bc2285d0c75941b42`; target-host acceptance remains required.


### AUDIT-017 — model-store copy could proceed without proving the private server had stopped
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** High / persistent model-store integrity
- **Evidence:** The installer ignored `systemctl stop` failure and treated any nonzero `systemctl is-active --quiet` result as proof the service was inactive. A systemd/D-Bus query error or an orphaned `ollama` process could therefore be mistaken for a stopped server, allowing the copier to mutate the live private model store.
- **Fix:** Capture the prior active/enabled states as explicit strings and fail on unrecognized/query-error states. After requesting stop, accept only inactive/failed/unknown unit states, then verify there is no `ollama` process owned by `r2`; any ambiguous state blocks copying.
- **Regression coverage:** The overnight service smoke test asserts that post-stop state and orphan-process checks occur before `copy_ollama_model.py` is invoked.
- **Status:** Source fix and regression assertion are committed; target-host installation still must demonstrate that systemd stops the private model server cleanly.

### AUDIT-018 — audio-output complaints were exempted as generic audio-topic discussion
- **File:** `r2.c`
- **Severity:** Medium / conversation recovery
- **Evidence:** The reply-misclassification guard intentionally exempts legitimate audio/transcription topics, but its output-problem detector recognized text/message/reply wording and not `your audio` or `your voice`. A complaint such as `Your audio is garbled.` therefore fell into the topic exemption and could let a reply that labels the user's message gibberish bypass recovery.
- **Fix:** Extend the output-problem detector to recognize explicit references to R2's output, including `your output`, `your audio`, `your voice`, `your speech`, and `your sound`, while retaining the issue-word requirement and existing audio-topic exemption for ordinary discussion.
- **Regression coverage:** Added a test where `Your audio is garbled.` followed by a gibberish-classifying reply must trigger recovery; the existing ordinary audio/transcription exemption test remains in place.
- **Status:** Source fix and focused regression test are committed; target-host voice/audio acceptance remains required.


### AUDIT-019 — vision JSON construction ignored allocation/insertion failures
- **File:** `Visual.c`
- **Severity:** Medium / vision reliability under memory pressure
- **Evidence:** The vision request builder and focus-region writer previously inserted JSON fields without checking json-c allocation or insertion return values. A partial request or focus record could proceed to serialization.
- **Fix:** Added checked object/array insertion helpers, routed request and focus fields through them, and made failed construction release only objects still owned by the builder before returning.
- **Regression coverage:** Added `tests/visual_payload_allocation_smoke.py` and wired it into the build workflow. It checks the expected payload fields, checked insertion helpers, failure cleanup, and absence of unchecked direct insertions.
- **Status:** Source fix and focused regression coverage passed in the full workflow at code/test head `ed013aac011f0ea238ab56af1f92ad054c22fa40` (91/91 steps; run [39229363923](https://github.com/Xylin48/R2-3PO/actions/runs/39229363923)). Physical camera inference still requires target-host acceptance.


### AUDIT-020 — startup-thread failure unwind was not exercised at runtime
- **File:** `r2.c`; regression harness and `.github/workflows/build.yml`.
- **Severity:** Medium / lifecycle reliability
- **Evidence:** The source-level regression checked that both thread-creation failure paths return to `main()`'s centralized cleanup, but did not actually trigger either failure after partial subsystem initialization.
- **Fix:** Added a dedicated test harness compiled with `R2_TEST_STARTUP_FAILURE_INJECTION`. CI now runs separate isolated processes for Hands-thread creation failure and autonomous-thinking/diary-thread creation failure; each requires initialization to fail and then invokes centralized shutdown. The production binary is compiled without the test-only injection define.
- **Validation:** Both injected runtime cases, the source-contract regression, and the strict production compile passed on code/test head `ed013aac011f0ea238ab56af1f92ad054c22fa40`; full workflow [39229363923](https://github.com/Xylin48/R2-3PO/actions/runs/39229363923) completed 91/91 steps successfully.
- **Remaining limitation:** This verifies the CI/Linux cleanup path, not fault injection on the target host or hardware-dependent startup.

## Current verification snapshot
The current code/test candidate passed all 91 workflow steps at `ed013aac011f0ea238ab56af1f92ad054c22fa40` (run [39229363923](https://github.com/Xylin48/R2-3PO/actions/runs/39229363923)). The subsequent commits update only audit documentation; the target-host acceptance checklist remains open. This audit remains deliberately bounded: source review and CI do not establish actual Qwen 3.5 9B behavior, GTX 1050 Ti allocation, physical device access, or the installed root-bootstrap state.
