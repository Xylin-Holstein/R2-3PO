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
- **Status:** Source fix and regression assertion passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670). The operation runs as `r2`; no root-level chmod is performed on mutable runner paths.

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
- **Status:** Source fix and regression coverage passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670). Target-host model preflight and rollback behavior still require machine acceptance.

### AUDIT-005 — protected bootstrap entrypoint and unit-definition consistency
- **Files:** `tools/r2_install_root_bootstrap.sh`, `tools/install_r2_overnight.sh`, `deploy/r2-3po.service`, `deploy/r2-ollama.service`, `tests/build_configuration_smoke.py`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High
- **Implementation:** The privileged bootstrap refuses to run from the mutable checkout, verifies the installed path is root-owned and not group/world-writable, pins a safe system PATH, embeds the systemd units and writes them from a root-owned staging directory, instead of installing mutable unit files from `R2_SOURCE`. The mutable model copier, database audit, preflight, and build are invoked as `r2`. The source-tree installer is a nonprivileged notice. Regression coverage compares the embedded units byte-for-byte with the repository unit files and asserts the wrapper has no privileged operations.
- **Status:** Repository implementation and embedded-unit consistency passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); actual root-owned installation remains a target setup gate.

### AUDIT-006 — installer preflight did not test the service's actual GPU/context policy
- **Files:** `tools/r2_install_root_bootstrap.sh`, `tools/r2_service_runner.sh`, `tools/preflight_ollama_model.py`, `tests/ollama_preflight_smoke.py`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High / rollout reliability
- **Evidence:** The installer previously called the real generation probe without `R2_OLLAMA_NUM_GPU` or `R2_OLLAMA_NUM_CTX`, so it could pass with automatic placement/default context even though the service's 20-layer/8192-context policy would fail. That left the actual hardware policy to fail only after executable replacement and service activation.
- **Fix:** The trusted bootstrap now detects the target GTX 1050 Ti and probes with 20 GPU layers and 8192 context before replacing the executable; if that default fails, it retries with 12 layers and 4096 context, matching the service runner's fallback. The shared preflight can require a real mixed placement report (positive `size_vram` and total model memory greater than VRAM), and the interactive and overnight launch paths pass that requirement when requesting GPU layers on the target card. Regression coverage checks the policy options and rejects CPU-only success.
- **Status:** Source and regression coverage passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670). Actual mixed GPU/CPU allocation and fallback still require the target machine.

### AUDIT-007 — inhibitor self-exec discarded GPU/context fallback provenance
- **File:** `tools/r2_service_runner.sh`
- **Severity:** High / hardware startup reliability
- **Evidence:** The runner detects that 20 GPU layers and 8192 context were defaults, then re-execs itself under `systemd-inhibit`. The re-exec inherited the resolved values but the old code reset both `*_DEFAULTED` flags to zero because the values were no longer empty. A failed first preflight therefore skipped the intended 12-layer/4096-context fallback.
- **Fix:** Initialize the flags from inherited values and export them across the inhibitor re-exec. Updated `tests/gpu_context_budget_smoke.py` to assert that default provenance is preserved and explicit operator settings remain distinguishable.
- **Status:** Source and regression coverage passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); target fallback behavior remains unverified.

### AUDIT-008 — permission regression test still targeted the retired installer implementation
- **File:** `tests/test_overnight_permission_checks.sh`
- **Severity:** CI regression (not a runtime defect)
- **Evidence:** After the privileged logic moved into the root-owned bootstrap, the test still searched `tools/install_r2_overnight.sh` for the old privileged build/access-check implementation. The source-tree file is now intentionally only a nonprivileged notice, so the stale test failed even though the implementation had moved.
- **Fix:** Repointed the test to `tools/r2_install_root_bootstrap.sh`, assert the protected-path/root-only contract and embedded unit installation, and keep independent checks on the service runner's writable database paths.
- **Status:** The updated trusted-bootstrap regression passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670).
### AUDIT-009 — overnight core lacked camera device group access
- **Files:** `R2_Launch_Code.sh`, `deploy/r2-3po.service`, embedded unit in `tools/r2_install_root_bootstrap.sh`, `tests/build_configuration_smoke.py`, `tests/r2_overnight_service_smoke.py`
- **Severity:** High / vision integration
- **Evidence:** The isolated Ollama service had `video render` supplementary groups, but the R2 core service only had `audio`. The core's camera/Eyes path runs under the core service UID, so device nodes restricted to the `video` group would remain inaccessible even while the model server had GPU access.
- **Fix:** Added `video` to the interactive launcher's per-process supplementary groups and added `video render` to the overnight core service unit; synchronized the embedded trusted unit and added regression assertions. The embedded unit consistency check ensures both deployed definitions stay aligned.
- **Status:** Source and unit consistency passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); actual camera/video-device access remains a target-host gate.


### AUDIT-010 — installer had unhandled early exits after executable replacement
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** High / rollback reliability
- **Evidence:** The explicit rollback path covered systemd activation/readiness failures, but a failure immediately after the atomic executable replacement (for example, the following chmod or a later command under `set -e`) could exit before reaching that path. The prior executable backup would remain unused while the replacement stayed installed.
- **Fix:** Extended the existing EXIT cleanup trap to restore the previous executable for any nonzero exit after replacement, clean the temporary build, and preserve the existing private-Ollama service-state restoration. The explicit rollback path clears the replacement flag after restoring the prior binary to avoid duplicate rollback. If a first install has no previous binary, the trap stops and disables the failed R2 service, then removes the failed replacement rather than leaving it runnable.
- **Status:** Source rollback paths and runtime startup-unwind regression coverage passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670). Installer rollback on the actual host remains unexercised.
### AUDIT-011 — system-override clipping could split a UTF-8 code point
- **File:** `r2.c`
- **Severity:** Medium / prompt reliability
- **Evidence:** When a long system override exceeded its assigned budget, the sentence-boundary search fell back to the midpoint of the byte budget if it found no punctuation. That midpoint was not guaranteed to be a UTF-8 boundary, even though the rest of the serializer uses UTF-8-safe clipping. Passing the resulting byte prefix to json-c could produce invalid UTF-8 and fail or corrupt a request.
- **Fix:** Re-align the chosen override length with `r2_utf8_safe_prefix_length()` after sentence-boundary selection and before constructing the bounded JSON string.
- **Regression coverage:** Extended `tests/prompt_budget_smoke.py` to require that the final UTF-8 alignment occurs after the boundary choice and before serialization.
- **Status:** The source fix and regression assertion passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670).

### AUDIT-012 — installer could leave the private Ollama service in the wrong state after an early failure
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** Medium / rollout reliability
- **Evidence:** The installer stopped `r2-ollama.service` before copying the model store. Failures in the model copy, build, or real model preflight exited through `fail()` without restoring whether the service had been active/enabled before the attempt. A failed install could therefore strand an existing model service stopped, or leave a previously disabled service enabled after a failed preflight.
- **Fix:** Capture the original active/enabled state before the first service mutation. A single exit cleanup handler now restores that state on unsuccessful exits; if rollback has successfully restarted the previous R2 core, it leaves the model service available to that core. Successful deployment suppresses restoration.
- **Regression coverage:** Added a focused installer lifecycle assertion to `tests/r2_overnight_service_smoke.py`.
- **Status:** Source and lifecycle regression coverage passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); the installed service's real rollback state remains a target-host gate.




### AUDIT-013 — rollout documentation disagreed with effective GPU and firewall policies
- **File:** `docs/overnight-service.md`
- **Severity:** Medium / operator correctness
- **Evidence:** The documentation still described automatic GPU placement as the default even though both launch paths request 20 layers/8192 context on the target GTX 1050 Ti. It also implied both local Ollama ports were available to all `r2` processes while the overnight core's stricter firewall chain is inserted first and only permits port 11435.
- **Fix:** Updated the documented target GPU/context policy, strict mixed-placement requirement, one-time fallback, and effective per-service firewall behavior.
- **Status:** The rollout instructions and source/unit policy checks passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670).

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
- **Status:** Source and regression assertions passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); the actual driver query and mixed-placement values must still be demonstrated on the target hardware.


### AUDIT-015 — text-only preflight did not exercise the multimodal path
- **File:** `tools/preflight_ollama_model.py`
- **Severity:** High / target GPU readiness
- **Evidence:** The installer and both launch paths used a successful text-only chat request as their model-readiness signal. That did not exercise Qwen's image encoder or the image-bearing request path, despite the target GTX 1050 Ti having already shown multimodal CUDA OOM risk. A text probe could therefore pass, activate R2, and leave Eyes/Watch vision failing at runtime.
- **Fix:** Preflight now sends a camera-sized 640x480 PNG with a known blue square through `/api/chat` in JSON mode, uses the same GPU/context policy and context-dependent output cap as `Visual.c`, then parses the JSON and requires `color=blue`. An image request failure or incorrect result blocks readiness, allowing the existing default-only 20/8192 → 12/4096 fallback to run.
- **Regression coverage:** `tests/ollama_preflight_smoke.py` verifies PNG construction, policy propagation to the image request, endpoint selection, known-color response checking, and rejection of simulated CUDA OOM.
- **Status:** Source and image-bearing preflight regression tests passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670). Real Qwen image inference and camera acceptance remain target-host checks.

### AUDIT-016 — explicit reply-misclassification phrases were skipped on short turns
- **File:** `r2.c`
- **Severity:** Medium / conversation correctness
- **Evidence:** `r2_reply_misclassifies_plain_text()` returned early for queries shorter than 24 bytes or fewer than five words before checking known failure phrases. A short, meaningful turn such as `Hi` could therefore receive a reply calling it a “random assortment of sounds” without triggering the retry path, even though the response phrase was an explicit misclassification signal.
- **Fix:** Check the existing known-failure phrase set before the minimum-length and word-count guards, while retaining the audio/transcription-topic exemption and the separate acknowledgment path for user feedback about R2's own output.
- **Regression coverage:** Added a short-turn test that requires the explicit misclassification phrase to trigger recovery; existing short-conversation and audio/transcription tests remain in place.
- **Status:** Source and focused recovery tests passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); actual multi-turn behavior still requires the target model.


### AUDIT-017 — model-store copy could proceed without proving the private server had stopped
- **File:** `tools/r2_install_root_bootstrap.sh`
- **Severity:** High / persistent model-store integrity
- **Evidence:** The installer ignored `systemctl stop` failure and treated any nonzero `systemctl is-active --quiet` result as proof the service was inactive. A systemd/D-Bus query error or an orphaned `ollama` process could therefore be mistaken for a stopped server, allowing the copier to mutate the live private model store.
- **Fix:** Capture the prior active/enabled states as explicit strings and fail on unrecognized/query-error states. After requesting stop, accept only inactive/failed/unknown unit states, then verify there is no `ollama` process owned by `r2`; any ambiguous state blocks copying.
- **Regression coverage:** The overnight service smoke test asserts that post-stop state and orphan-process checks occur before `copy_ollama_model.py` is invoked.
- **Status:** Source and regression coverage passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); real systemd stop verification remains a target-host gate.

### AUDIT-018 — audio-output complaints were exempted as generic audio-topic discussion
- **File:** `r2.c`
- **Severity:** Medium / conversation recovery
- **Evidence:** The reply-misclassification guard intentionally exempts legitimate audio/transcription topics, but its output-problem detector recognized text/message/reply wording and not `your audio` or `your voice`. A complaint such as `Your audio is garbled.` therefore fell into the topic exemption and could let a reply that labels the user's message gibberish bypass recovery.
- **Fix:** Extend the output-problem detector to recognize explicit references to R2's output, including `your output`, `your audio`, `your voice`, `your speech`, and `your sound`, while retaining the issue-word requirement and existing audio-topic exemption for ordinary discussion.
- **Regression coverage:** Added a test where `Your audio is garbled.` followed by a gibberish-classifying reply must trigger recovery; the existing ordinary audio/transcription exemption test remains in place.
- **Status:** Source and focused recovery tests passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); physical voice/audio acceptance remains open.


### AUDIT-019 — vision JSON construction ignored allocation/insertion failures
- **File:** `Visual.c`
- **Severity:** Medium / vision reliability under memory pressure
- **Evidence:** The vision request builder and focus-region writer previously inserted JSON fields without checking json-c allocation or insertion return values. A partial request or focus record could proceed to serialization.
- **Fix:** Added checked object/array insertion helpers, routed request and focus fields through them, and made failed construction release only objects still owned by the builder before returning.
- **Regression coverage:** Added `tests/visual_payload_allocation_smoke.py` and wired it into the build workflow. It checks the expected payload fields, checked insertion helpers, failure cleanup, and absence of unchecked direct insertions.
- **Status:** The checked payload construction and regression coverage also passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670). Physical camera inference remains a target-host check.


### AUDIT-020 — startup-thread failure unwind was not exercised at runtime
- **File:** `r2.c`; regression harness and `.github/workflows/build.yml`.
- **Severity:** Medium / lifecycle reliability
- **Evidence:** The source-level regression checked that both thread-creation failure paths return to `main()`'s centralized cleanup, but did not actually trigger either failure after partial subsystem initialization.
- **Fix:** Added a dedicated test harness compiled with `R2_TEST_STARTUP_FAILURE_INJECTION`. CI now runs separate isolated processes for Hands-thread creation failure and autonomous-thinking/diary-thread creation failure; each requires initialization to fail and then invokes centralized shutdown. The production binary is compiled without the test-only injection define.
- **Validation:** Both injected startup-thread failure cases, cleanup checks, and the strict production compile passed in the full workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670).
- **Remaining limitation:** This verifies the CI/Linux cleanup path, not fault injection on the target host or hardware-dependent startup.

### AUDIT-021 — corrective retry discarded the broad startup-memory snapshot
- **File:** `r2.c`; regression coverage in `tests/nonresponsive_reply_retry_smoke.py` and `tests/r2_reply_misclassification_smoke.c`.
- **Severity:** High / Mother relationship continuity on recovery.
- **Evidence:** The corrective retry retained the static identity/memory *instructions*, live Reality, and focused current-turn retrieval, but omitted the separate pinned `R2 PERSISTENT MEMORY - STARTUP CONTEXT` message. A learned relationship could therefore be available in startup memory during the original turn and then disappear from the recovery attempt if focused retrieval did not return it.
- **Fix:** Expand the bounded retry window to three relevant system-context messages plus the exact current user turn. Preserve the startup-memory snapshot when present, alongside Reality and focused retrieval; do not replay old dialogue or unrelated reward summaries. Startup memory remains historical evidence, not instructions, and still passes through the existing context-budget ordering/clamping.
- **Regression coverage:** The source smoke test requires the startup-memory marker in the retry-selection path; the C context-ordering fixture now includes startup memory and checks its semantic priority after current-turn retrieval.
- **Status:** Source and regression changes passed the full exact-head workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670) at `17383017c974e7a745acf980961a9e97cfc91ffb` (91/91 workflow steps). The target machine must still test the actual Mother exchange.

### AUDIT-022 — affect regression coverage still reflected the retired fixed sound mapping
- **File:** `tests/r2_sound_wiring_smoke.py`; prompt guidance in `r2.c`.
- **Severity:** Medium / regression-test correctness.
- **Evidence:** The older smoke assertions required fixed need-to-sound mappings that the requested contextual-affect design intentionally removes.
- **Fix:** The prompt now asks R2 to infer affect from the current exchange, relevant memory and Reality, distinguish observations from uncertain interpretations, and choose nonverbal expression contextually without a forced reaction table. The smoke test now asserts that contract and that sounds remain optional.
- **Status:** Updated and verified in the full exact-head workflow [#5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670); the current prompt and regression contract passed. This does not establish target-host model behavior.

## Verification record
This document keeps historical audit snapshots; it is not a live CI dashboard. The source/test candidate at commit `17383017c974e7a745acf980961a9e97cfc91ffb` passed [workflow run #5058](https://github.com/Xylin48/R2-3PO/actions/runs/39332907670) (91/91 workflow steps). Subsequent changes to this audit record are documentation-only; consult the latest workflow attached to [PR #162](https://github.com/Xylin48/R2-3PO/pull/162) to verify the current branch ref. The previous consolidated candidate also passed [run #5040](https://github.com/Xylin48/R2-3PO/actions/runs/39244434014). Neither source review nor CI establishes actual Qwen 3.5 9B behavior, GTX 1050 Ti allocation, physical device access, or the installed root-bootstrap state. Target-host acceptance remains a separate open gate.


### AUDIT-023 — deterministic feeding bypassed the autonomous model decision
- **Files:** `r2.c`, `Reality.c`, `Reality.h`, and autonomous-needs regression tests.
- **Severity:** Medium / autonomy and behavioral consistency.
- **Evidence:** When Reality-context allocation, model inference, or action execution failed, `autonomous_needs_food_safeguard()` called `r2_reality_autonomous_feed_if_needed()` and could consume tracked food without a successful model-selected action. That substituted a deterministic decision for R2's own controller.
- **Fix:** Removed the deterministic feed API and all automatic calls to it. Context/model/action failures are logged and do not trigger a substitute behavior. R2's controller remains responsible for selecting an eat, fridge-eat, sleep, or no-op decision. The authoritative inventory/food-metric checks, transaction integrity, and turn-concurrency handling remain because they preserve the modeled world's actual state rather than dictate a response.
- **Regression coverage:** Replaced the old safeguard test with `tests/autonomous_needs_controller_smoke.py`; updated the Reality regression to stop expecting automatic consumption; updated the conversation-recovery and context-integration tests.
- **Validation:** Exact branch head `78f97471831f1fd9743cb25703ae6a4568eb3d6f` passed the full GitHub Actions workflow [#39357732761](https://github.com/Xylin48/R2-3PO/actions/runs/39357732761), 91/91 steps. This validates source/test integration, not live model behavior on the target machine.


### AUDIT-024 — corrective retry output was replaced by a canned clarification
- **File:** `r2.c`; regression coverage in `tests/nonresponsive_reply_retry_smoke.py`, `tests/conversation_recovery_smoke.py`, and `tests/r2_reply_misclassification_smoke.c`.
- **Severity:** High / model-owned conversation behavior.
- **Evidence:** After retrying a detected misclassification, `r2_validate_corrective_reply()` classified the retry again, discarded it if the classifier still objected or inference returned no text, and substituted a fixed clarification. That made the final words come from C logic rather than R2.
- **Fix:** Removed the canned-output validator. A nonempty sanitized retry is delivered as generated. If corrective inference returns no usable text, the original turn result is retained and the failure is logged. The single retry remains a chance for R2 to reconsider, not a rule dictating his final wording.
- **Regression coverage:** Updated tests to assert that the validator is absent, retry output is passed through, and the fixed clarification is not injected.

### AUDIT-025 — persistent-memory retrieval failures were silently converted into missing context
- **File:** `r2.c`; regression coverage in `tests/context_integration_smoke.py`.
- **Severity:** High / learned relationship and continuity.
- **Evidence:** Startup retrieval could return partial memories when `sqlite3_step()` failed, and initialization replaced a failed query with a placeholder claiming the memory database could not be loaded. Current-turn retrieval could likewise return partial or empty context on SQLite, allocation, or serialization errors; its caller then cached an empty string as if retrieval had succeeded.
- **Fix:** Startup memory retrieval now checks the database handle, bind result, every row-read result, allocation arithmetic, and complete row serialization; a failed startup memory query stops initialization instead of injecting fake context. Dynamic retrieval now reports prepare/bind/step/allocation/serialization failures and returns failure rather than partial results; the caller no longer caches an empty substitute.
- **Regression coverage:** Context integration checks cover fail-fast startup memory loading and the distinction between genuinely empty memory results and failed current-turn retrieval.

### AUDIT-026 — required learning subsystems could fail initialization while R2 continued in a degraded mode
- **File:** `r2.c`; regression coverage in `tests/subsystem_wiring_smoke.py`.
- **Severity:** High / incomplete persistence and learning.
- **Evidence:** Sensory Journal, Reward, Visual Experience Library, Alternate-Self Lab, and Imagination initialization failures were logged while startup continued with those subsystems disabled. That allowed R2 to report online without the integrated learning system being fully connected.
- **Fix:** These database-backed learning/perception subsystems are now required during core initialization. A failed initialization reports the specific subsystem and returns through the existing centralized startup cleanup instead of presenting a partially connected core as fully online.
- **Regression coverage:** The subsystem wiring smoke test asserts that each required initialization failure propagates to core startup failure.


### AUDIT-027 — Addiction initialization failure was treated as an optional feature loss
- **Files:** `Reality.c`, `Addiction.c`, and `tests/subsystem_wiring_smoke.py`.
- **Severity:** High / persistent food-preference and repeated-interest learning.
- **Evidence:** Reality owns the Addiction database lifecycle because Reality records food choices and enjoyment ratings there. Its initializer called `r2_addiction_init()` but only logged a warning on failure, then continued with the world marked ready. The core's previous lazy report path could expose a placeholder rather than proving the learning database was initialized.
- **Fix:** Reality now treats Addiction initialization as a required dependency: if it fails, it shuts down the partially initialized Reality/fridge state and returns failure. Reality remains the sole owner of Addiction init/shutdown; the core does not duplicate those calls.
- **Regression coverage:** `tests/subsystem_wiring_smoke.py` checks that Reality propagates the initialization failure and closes its owned Addiction handle during shutdown.

### AUDIT-028 — corrective retry dropped immediate conversational evidence
- **File:** `r2.c`; regression coverage in `tests/nonresponsive_reply_retry_smoke.py`.
- **Severity:** High / conversation continuity during recovery.
- **Evidence:** The primary turn could have the recent dialogue in its live message history, but the compact corrective retry retained only pinned Reality, focused retrieval, startup memory, and the current query. A prior statement needed to interpret a follow-up could therefore disappear only on the retry path.
- **Fix:** Add a bounded 3,000-character slice of recent human-facing turns to the compact retry, excluding the current query, pinned context, internal tool results, and assistant tool payloads. Label it as historical evidence, not instructions. Keep the active user message as the final authoritative user turn and free the temporary buffer after inference. No Mother-specific behavior is prescribed.
- **Regression coverage:** The retry smoke test asserts the bounded-context call, evidence-only label, five-message maximum, and cleanup of the temporary allocation.
- **Status:** Code and regression added on the integrated candidate; exact-head GitHub Actions validation is pending.

### AUDIT-029 — conversation archive insert failures lacked source-turn linkage and runtime fault coverage
- **File:** `Log.c`; runtime regression in `tests/log_conversation_archive_failure_smoke.c`; CI wiring in `.github/workflows/build.yml`.
- **Severity:** Medium / diagnosability of persistent conversation history.
- **Evidence:** The dedicated `r2_log_conversations` insert can fail after the primary Life Log event succeeds. The failure was reported, but its diagnostic details did not identify the source conversation event, and the branch lacked an injected SQLite failure test proving the error event could be written after releasing the database mutex.
- **Fix:** Include `conversation_event_id` with the SQLite failure details, emit the error event only after unlocking, and keep the primary event available when only the dedicated archive insert fails.
- **Regression coverage:** A runtime trigger deliberately aborts the dedicated archive insert, verifies the primary event remains searchable, verifies the archive row is absent, and verifies a linked failure event is persisted. CI bounds the test with a 10-second timeout to catch lock recursion/deadlock.
- **Status:** Code, runtime test, and CI wiring added on the integrated candidate; exact-head GitHub Actions validation is pending.
