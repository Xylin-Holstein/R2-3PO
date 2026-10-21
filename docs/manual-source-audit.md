# Manual source audit — rollout candidate

Candidate branch: `integrate/r2-hardware-ready-2026-10-20`
PR: [#167](https://github.com/Xylin48/R2-3PO/pull/167) (open; do not merge while target-host rollout blockers remain)

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
- **Status:** Integrated head `9be23f212168691c7f67a2f421319f9af0cafcd5` passed exact-head GitHub Actions workflow [#5472](https://github.com/Xylin48/R2-3PO/actions/runs/39398984552), all 94/94 steps. This validates the source/test suite, not live Ollama response quality on the target host.

### AUDIT-029 — conversation archive insert failures lacked source-turn linkage and runtime fault coverage
- **File:** `Log.c`; runtime regression in `tests/log_conversation_archive_failure_smoke.c`; CI wiring in `.github/workflows/build.yml`.
- **Severity:** Medium / diagnosability of persistent conversation history.
- **Evidence:** The dedicated `r2_log_conversations` insert can fail after the primary Life Log event succeeds. The failure was reported, but its diagnostic details did not identify the source conversation event, and the branch lacked an injected SQLite failure test proving the error event could be written after releasing the database mutex.
- **Fix:** Include `conversation_event_id` with the SQLite failure details, emit the error event only after unlocking, and keep the primary event available when only the dedicated archive insert fails.
- **Regression coverage:** A runtime trigger deliberately aborts the dedicated archive insert, verifies the primary event remains searchable, verifies the archive row is absent, and verifies a linked failure event is persisted. CI bounds the test with a 10-second timeout to catch lock recursion/deadlock.
- **Status:** Integrated head `9be23f212168691c7f67a2f421319f9af0cafcd5` passed exact-head GitHub Actions workflow [#5472](https://github.com/Xylin48/R2-3PO/actions/runs/39398984552), all 94/94 steps. The SQLite trigger test verifies the dedicated archive-failure path and lock release.

### AUDIT-030 — dedicated conversation archive was write-only across process restarts
- **Files:** `Log.c`, `Log.h`, `r2.c`; runtime coverage in `tests/log_conversation_archive_failure_smoke.c`; static coverage in `tests/archive_prompt_smoke.py` and `tests/nonresponsive_reply_retry_smoke.py`.
- **Severity:** High / conversation continuity after restart.
- **Evidence:** The dedicated `r2_log_conversations` table stored exact user/assistant turns, but no read path restored recent rows into startup context. Topic-based Life Log search could retrieve matching events, but generic follow-ups about the immediately preceding exchange had no bounded transcript context after a process restart.
- **Fix:** Add `r2_log_recent_conversation_context()`, which reads only the newest eight complete rows, filters empty legacy rows, preserves chronological order, clips individual text at UTF-8 boundaries, and enforces an 8,192-character API ceiling. Startup injects at most 3,000 characters as historical evidence, not instructions. Corrective retries can retain that persisted context alongside live Reality, focused retrieval, startup memory, a bounded current-session dialogue slice, and the exact current user turn.
- **Regression coverage:** The runtime test writes two distinct turns, shuts down/reopens the Life Log, then asserts the persisted context contains both turns in chronological order and within the 3,000-character bound. It then injects a SQLite archive-write failure and verifies the primary event and linked failure diagnostic.
- **Status:** Integrated head `9be23f212168691c7f67a2f421319f9af0cafcd5` passed exact-head GitHub Actions workflow [#5472](https://github.com/Xylin48/R2-3PO/actions/runs/39398984552), all 94/94 steps, including the restart-continuity and archive-failure runtime checks.


### AUDIT-031 — generic clarification refusals could bypass the corrective retry
- **File:** `r2.c`; regression coverage in `tests/r2_reply_misclassification_smoke.c`.
- **Severity:** High / ordinary conversational turns can be misclassified as unintelligible.
- **Evidence:** The detector recognized explicit failure wording such as “garbled” and “gibberish,” but a generic refusal such as “I'm not sure what you mean. Could you please clarify your message?” could bypass recovery even for a long, grammatical multi-part message.
- **Fix:** Add a separate generic-clarification detector after the ordinary query-length and word-count guards. Keep explicit output-error feedback on its existing short-message path; exclude direct user requests for clarification/rephrasing and avoid retrying short greetings automatically.
- **Regression coverage:** The C smoke test covers a substantive Mother/room/pockets message that receives a generic clarification refusal, an explicit user clarification request, and a short greeting.
- **Status:** Exact integration head `770e3e3756af90c78e0d5a821ab31a0b4e847806` passed GitHub Actions workflow [#5492](https://github.com/Xylin48/R2-3PO/actions/runs/39474767159), including the production reply-misclassification test, strict build, and remaining regression suite. Target-host Ollama behavior remains unverified.


## Superseding inference-budget finding — 2026-10-20

The earlier 20-layer / 8192-context default and its post-failure 12-layer / 4096-context retry described above are historical and no longer describe the current launch path. The target host has a 4 GiB GTX 1050 Ti and the multimodal Qwen 3.5 9B inference path has already shown CUDA allocation failure under the larger initial policy. The interactive launcher, overnight runner, and trusted root installer now begin with 12 GPU layers and 4096 context tokens, avoiding the known oversized first allocation; they preserve explicit operator settings where the launch path supports them and stop startup if the real text-and-image preflight fails. The GPU-policy regression test now checks all three entry paths. This source change still requires a fresh hardware run to establish that the smaller budget is sufficient and that Ollama reports mixed CPU/GPU placement.


### AUDIT-032 — deployment preflight omitted the shared inference probe from its source inventory
- **Files:** `tools/preflight_r2_hardware.sh`, `tests/r2_hardware_preflight_smoke.py`.
- **Severity:** Medium / startup diagnostics and incomplete preflight.
- **Evidence:** The launcher and overnight runner require `tools/preflight_ollama_model.py` to run real text-and-image generations before starting R2, but the read-only deployment inventory did not report that helper as missing. Startup would eventually stop, but the first preflight could incorrectly appear complete.
- **Fix:** Add the shared inference probe to the deployment file inventory and regression contract so missing-file failures are surfaced in the read-only preflight itself.
- **Validation:** Exact integration head `b0a9f7c34a011bcc4b954c3c468a92bc39f2b022` passed workflow [#5561](https://github.com/Xylin48/R2-3PO/actions/runs/39492437986), all 94 steps.

### AUDIT-033 — target-host RAM headroom was not reported by the inference probe
- **Files:** `tools/preflight_ollama_model.py`, `tests/ollama_preflight_smoke.py`, `docs/rollout-readiness.md`.
- **Severity:** Medium / hardware acceptance evidence was incomplete.
- **Evidence:** The probe verified real text and image-bearing generations and queried Ollama's reported VRAM placement, but did not capture host `MemAvailable` while the model remained warm. That left the user's approximately 3.8 GiB free-RAM target without a direct measurement in the same acceptance output.
- **Fix:** Report Linux `MemAvailable` after both generations and document recording that value beside the `/api/ps` mixed-placement evidence. This is a diagnostic target, not an unconditional startup gate.
- **Validation:** Exact integration head `b0a9f7c34a011bcc4b954c3c468a92bc39f2b022` passed workflow [#5561](https://github.com/Xylin48/R2-3PO/actions/runs/39492437986), all 94 steps. The target host still needs a fresh inference run to record its actual RAM headroom.

### AUDIT-034 — runtime Ollama HTTP diagnostics truncated the server error
- **Files:** `r2.c`, `tests/ollama_request_config_smoke.py`.
- **Severity:** Medium / diagnosis of inference failures.
- **Evidence:** The preflight reports the Ollama error detail, but R2's runtime HTTP failure path printed only the first 400 characters of the response body. A longer CUDA allocation/KV-cache error could therefore be cut off in the terminal even though the bounded response buffer and persistent event already retained the body.
- **Fix:** Runtime HTTP failures now print the response byte count and the complete response body accepted by the existing bounded response reader; a regression contract rejects the old precision-limited format.
- **Validation:** Added a focused regression assertion. CI status for this new commit has not yet been reported by GitHub; the target-host GPU/Ollama inference still has not been run in this session.

### AUDIT-035 — 12-layer GTX 1050 Ti policy still hit multimodal KV-cache allocation failure
- **Files:** `R2_Launch_Code.sh`, `tools/r2_service_runner.sh`, `tools/r2_install_root_bootstrap.sh`, GPU-policy smoke tests, README and rollout documentation.
- **Severity:** High / startup blocker on the target host.
- **Evidence:** Prior target-host reports include a Qwen 3.5 9B CUDA allocation failure involving the KV cache and a roughly 335 MiB allocation after the earlier 20-layer / 8192-context policy had already been reduced to 12 layers / 4096 context.
- **Fix:** Keep the 4096-token context but lower the GTX 1050 Ti default to 8 GPU layers across interactive launch, overnight service, and trusted installer preflight. This moves more layer memory into system RAM and leaves additional VRAM for multimodal cache/vision allocations. Updated the installer’s pinned bootstrap blob and commit together with the documentation and policy tests.
- **Validation:** The previous diagnostic-only commit passed workflow run #5573 (94/94 steps). The new 8-layer policy has not yet completed its own CI run and has not been verified by a live target-host inference; neither is claimed as complete.

### AUDIT-036 — Startup persistent memories could be starved by the system-prompt budget
- **Files:** `r2.c`, `tests/startup_memory_budget_smoke.py`.
- **Severity:** High / learned identity and continuity regression under a tight context window.
- **Evidence:** `r2_compute_system_context_budget()` previously assigned every byte left after the identity, Reality, retrieval, and capability reserves to `override_budget`. The serializer begins `total_chars` at the system override length and previously assigned every remaining byte to that override before it considered startup memories (priority 5) or the separate persistent-learning context (priority 6). When earlier budgets were filled, the newest 100 database memories and the learning context built from live reward/addiction state could receive zero bytes. That can make a learned relationship, persistent fact, or learning/needs context disappear from the model's actual prompt even while the underlying databases and context messages exist in process memory.
- **Fix:** Reserve bounded, explicit shares for `R2 PERSISTENT MEMORY - STARTUP CONTEXT` and `PERSISTENT LEARNING CONTEXT` before computing the override budget, and cap each section to its own reserve so one cannot crowd out the other. Shorten the startup block's explanatory header so the reserved bytes reach actual database records rather than boilerplate. Keep the live reward and repeated-interest summaries balanced and compact so a small prompt cannot include reward context while clipping the addiction/repeated-interest database entirely. When reward events exist, choose the newest `Recent:` record instead of spending the compact reward slice on the generic reward-policy header. Recent conversation archives remain lower priority. This is a general database-evidence allocation, not a hard-coded identity or relationship rule. The current user turn and current-turn retrieved evidence retain their existing independent path and priority.
- **Validation:** Added a regression smoke test for the separate startup-memory and persistent-learning reserves and priority contract. CI has not yet validated this change; target-host inference and database round-trip compatibility remain unverified and are not claimed complete.

### AUDIT-037 — Common “trouble understanding” replies bypassed conversation recovery
- **Files:** `r2.c`, `tests/r2_reply_misclassification_smoke.c`.
- **Severity:** High / repeatable conversational nonresponse.
- **Evidence:** The existing recovery classifier recognized several generic clarification refusals, but did not recognize common variants such as “I'm having trouble understanding your message.” On a substantial, coherent turn, that answer could pass through as a final response without the corrective inference.
- **Fix:** Added the missing trouble/difficulty-understanding and couldn't-understand variants to the existing generic clarification detector. The existing exemptions for genuine user clarification requests and short greetings remain in place. Added a production-guard regression case for a clear multi-part message.
- **Validation:** Included in the current integration CI candidate; results are pending. This tests the recovery decision path, not a live Qwen inference on the target PC.

### AUDIT-038 — Corrective retry dropped live learning context; archive priority loop skipped priority 8
- **Files:** `r2.c`; regressions in `tests/conversation_recovery_smoke.py` and `tests/context_integration_smoke.py`.
- **Severity:** High for consistent learned behavior; Medium for older conversation continuity.
- **Manual source finding 1:** `r2_is_retry_context_message()` recognized live Reality, current-turn retrieval, startup memory, and recent persisted dialogue, but omitted `PERSISTENT LEARNING CONTEXT`. Therefore, when R2 generated a generic “trouble understanding” response and entered corrective retry, the retry discarded the live reward and repeated-interest summary even though the normal request had it.
- **Fix 1:** Classify the live learning context as retry evidence and increase the compact retry message array/selection from four to five independent context sources, retaining space for recent dialogue and the exact current user turn.
- **Manual source finding 2:** `r2_inference_system_priority()` assigns archived conversation context priority 8, and `r2_order_inference_messages()` iterates priorities 0–8. The serializer budget loop incorrectly used `priority < 8`, so priority-8 archive messages could never be selected. The existing regression test passed because it searched the whole `r2.c` file and matched the ordering helper's correct `priority < 9` loop instead of checking the serializer itself.
- **Fix 2:** Extend the serializer loop through priority 8 and scope the regression assertion specifically to `ollama_chat_with_limit()`, so the test cannot be satisfied by an unrelated loop.
- **Validation:** New targeted checks are in the current integration candidate. This is a manually identified control-flow omission that a compile/syntax-only test would not catch. Awaiting exact-head CI; no claim of target-host validation.
 
### AUDIT-039 — Identity and conversation contracts were silently clipped; startup selection could hide durable relationship evidence
- **Files:** `r2.c`; regressions in `tests/prompt_budget_smoke.py`, `tests/qwen35_model_smoke.py`, `tests/current_turn_context_smoke.py`, `tests/conversation_recovery_smoke.py`, `tests/nonresponsive_reply_retry_smoke.py`, `tests/affect_mother_conversation_smoke.py`, `tests/startup_memory_budget_smoke.py`, and `tests/model_reference_consistency_smoke.py`.
- **Severity:** High / directly affects identity continuity and message comprehension.
- **Manual source finding 1:** The first static identity/memory section was approximately 4,860 decoded characters, while the serializer reserved about 1,000–1,500 characters for it on the target's small context. The full prompt passed the overall prompt-size test, but the high-priority identity section could still be cut mid-contract.
- **Fix 1:** Rewrote the core identity/memory section to 991 decoded characters while preserving R2 identity, actual speaker roles, historical-vs-current evidence, persistent-memory behavior, and live-vs-prior visual grounding. Added a test for this specific section's decoded length and required semantics.
- **Manual source finding 2:** The normal conversation override was approximately 4,321 characters, and the corrective retry built another long override. Both were passed through the same leftover-budget limiter, so the model could receive only a prefix and lose critical instructions near the end, including tool-result semantics and corrective guidance.
- **Fix 2:** Reduced the normal conversation contract to 256 decoded characters and the corrective retry contract to 255, both within the 263-character smallest tested override reserve. The normal contract preserves current-message focus, memory grounding, speaker roles, tool-result-vs-user-speech handling, non-repetition of completed tool results, typed-text grounding, and reasoning privacy. The retry contract explicitly treats complaints about R2's prior reply as feedback and corrects misreadings.
- **Manual source finding 3:** The retry selector omitted `PERSISTENT LEARNING CONTEXT` and `PAST CONVERSATION ARCHIVE — CONTEXT ONLY.`; its four-context selection and six-element array could not preserve the full set of durable sources alongside recent dialogue and the exact current user turn.
- **Fix 3:** Retry context selection now recognizes Reality, current-turn retrieval, startup memory, recent persisted dialogue, live learning, and archived dialogue; it has six context slots plus the recent-context slice and exact current user turn.
- **Manual source finding 4:** Startup memory SQL sorted only by `updated_at DESC`, while the serializer later truncates the startup block at a small record-boundary budget. A burst of recent miscellaneous records could push older but durable `self`, `identity`, `relationship`, or `person` memories out of the model's context.
- **Fix 4:** Startup selection prioritizes the existing generic memory categories `self`/`identity`, `relationship`, `person`, then preference/knowledge/experience and other records, using recency within each category. No database schema or stored user relationship was hard-coded; the startup heading now accurately says the records are prioritized rather than globally newest.
- **Validation status:** The integration suite has exposed stale assertions that depended on the old verbose prompt wording; those tests were updated to assert the compact contract's behavior. A fresh exact-head CI result is still required. No live inference or existing-user-database test has been run on the target PC.
 
