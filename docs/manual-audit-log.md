# R2-3PO manual audit log

This log records source-level findings and their verification status. A green workflow is evidence for the listed regressions, not proof of live model or hardware behavior.

## Audit batch: 2026-10-18 — conversation response contract

### R2-AUD-001 — Incomplete or wrong-role Ollama response could be treated as an answer

- **Area:** `r2.c`, non-streaming `/api/chat` response parsing.
- **Finding:** the parser copied `message.content` without requiring a string `message.role` equal to `assistant` or a boolean `done` equal to `true`. A syntactically valid but incomplete or malformed response could therefore enter the user-facing conversation as though it were a finished answer.
- **Fix:** accept only the expected JSON types, validate the assistant/completion contract, emit a structured error event on rejection, and return failure so the existing conversation retry/fallback path handles it visibly.
- **Regression coverage:** the production predicate is exercised by `tests/r2_reply_misclassification_smoke.c` for a valid assistant completion, a wrong role, an unfinished completion, and missing metadata. `tests/ollama_request_config_smoke.py` checks the parser's JSON-type validation and fail-closed path.
- **Status:** implemented and covered by the full workflow on commit `0159635cd31b0b1b2a5dfd49a90846d2b7b59e1c` (87/87 steps passed). Target-host behavior is not yet verified.

### R2-AUD-002 — GPU offload regression test had drifted from the vision implementation

- **Area:** `tests/ollama_gpu_offload_smoke.py` versus `Visual.c`.
- **Finding:** the test required the old direct `json_object_object_add(options, "num_gpu", ...)` expression, although `Visual.c` had already moved to a checked, transactional JSON insertion helper. The failure was in a stale source assertion, not evidence that vision omitted the GPU setting.
- **Fix:** update the assertion to require the checked helper and the actual `num_gpu` value construction, independent of the source's line wrapping.
- **Status:** corrected and covered by the full workflow on commit `0159635cd31b0b1b2a5dfd49a90846d2b7b59e1c` (87/87 steps passed).


### R2-AUD-003 — Overnight installer test expected the retired systemd probe form

- **Area:** `tests/r2_overnight_service_smoke.py` versus `tools/r2_install_root_bootstrap.sh`.
- **Finding:** the regression test still required `systemctl is-active --quiet` / `is-enabled --quiet`, but the bootstrap intentionally captures the full service states to distinguish inactive, failed, unknown, masked, static, and other legitimate states before changing anything.
- **Fix:** assert the current captured-state commands and their active/enabled classification branches rather than the retired probe syntax.
- **Status:** corrected; full workflow validation is pending for the current branch head.


### R2-AUD-004 — Overnight installer notice was not executable in the Git tree

- **Area:** Git file mode for `tools/install_r2_overnight.sh`.
- **Finding:** the source-tree notice had a valid Bash shebang and content, but its committed mode was `100644`; the permission regression test correctly rejected it because the documented direct invocation requires an executable file.
- **Fix:** restored the Git tree mode to `100755` without changing the script contents.
- **Status:** mode corrected in commit `501e101279622310d82ae5c0184d50aba73b7580`; full workflow passed on descendant commit `0159635cd31b0b1b2a5dfd49a90846d2b7b59e1c` (87/87 steps).



### R2-AUD-005 — Physical money balance replacement is not failure-atomic

- **Area:** `Reality.c`, `money_set_dir_balance()`, used by cash receipt, deposit/withdrawal, purchase, and migration paths.
- **Finding:** the former implementation removed authoritative bills before replacement files were safely available. A write failure could therefore destroy the prior balance.
- **Fix:** replacement files are staged before the authoritative set moves. Phase transitions are written through a temporary marker and atomic rename; transaction and money directories are synced at key boundaries. Backups are retained whenever rollback cannot be completed. Startup recovery distinguishes partial backup movement, partial installation, and committed replacement.
- **Regression coverage:** deterministic staging failure preserves the original $5 balance; a failure after the first backup move exercises synchronous rollback; a simulated restart during installation verifies the original authoritative bills are restored.
- **Status:** source and regressions passed both complete workflows on code commit `9fe05d0e000243b4b1ba9b515c39f68259e287bd` (87/87 steps each; push [39187194766](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39187194766), pull request [39187198480](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39187198480)). Actual host power-loss/filesystem durability remains unverified.

### R2-AUD-006 — Wallet-to-piggybank transfers were not recoverable as one operation

- **Area:** `Reality.c`, `money_transfer()`, covering deposits and withdrawals.
- **Finding:** wallet and piggybank each had an individually recoverable file replacement, but the overall transfer changed two separate directories sequentially. A crash after the first replacement and before the second could leave the transfer half-applied.
- **Fix:** added a durable SQLite transfer journal recording both old and proposed balances before either physical balance changes. A pending journal restores both old balances on synchronous failure or startup; a committed journal keeps the completed transfer and can be cleaned on a later start. The journal remains until recovery/cleanup succeeds.
- **Regression coverage:** deterministic failure after wallet replacement verifies both balances return to their original values; a restart fixture inserts a pending journal and partial new physical balances, then verifies startup restores the original $5 cash/$0 bank state.
- **Status:** implementation and regression passed both complete workflows on code commit `9fe05d0e000243b4b1ba9b515c39f68259e287bd` (87/87 steps each). Target-host power-loss behavior remains unverified.

### R2-AUD-007 — Purchase money and inventory persistence are not crash-atomic

- **Area:** `Reality.c`, `r2_reality_buy_item()`, spanning physical Wallet/piggybank files and Reality/fridge inventory storage.
- **Finding:** the purchase first persists the money deduction, then separately calls `r2_reality_add_item()`. Synchronous inventory failure attempts to restore the old balance, but a process crash after the deduction and before the item insertion can leave R2 charged without receiving the item. The transfer journal added for deposits/withdrawals does not yet cover purchases.
- **Fix:** added an additive `r2_purchase_journal` table recording old/proposed cash and bank balances, the intended item/container, pre-purchase quantity, and phase before any physical deduction. Startup recovery runs after both Reality and fridge databases are available. It checks whether the intended inventory stack advanced in the intended container: if not, it restores both old physical balances; if the item persisted, it retains the charge and reconciles the physical balances to the proposed values. The journal is cleared only after reconciliation or successful completion.
- **Concurrency/lifecycle fix:** a dedicated purchase mutex serializes the entire cross-database purchase, all other money mutations, and Reality shutdown. This prevents a concurrent deposit/withdrawal or shutdown from treating an active purchase as abandoned or closing its databases mid-commit. The committed journal is retained until the physical balances have been re-read successfully.
- **Regression coverage:** `tests/money_wallet_smoke.c` injects interruption before item persistence and after item persistence for both regular pocket inventory and the separate fridge database. Restart recovery verifies refund when no item was delivered and preserves the charge when the item exists. The tests also verify that the remaining money and inventory fixtures remain correct for subsequent purchases.
- **Status:** implemented and passed both complete workflows on code commit `d939b13af6e78441e2f9e967deb2e2b92be3dcad` (87/87 steps each): push [39188485522](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39188485522) and pull request [39188481756](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39188481756). These are deterministic interruption/restart tests, not physical power-loss tests. Target-host filesystem durability remains unverified.

### R2-AUD-008 — Thread-creation failure can leave initialized subsystems open

- **Area:** `r2.c`, `r2_init()` failure paths after Reality, Eyes, Ears, Visual, Life Log, Reward, Sensory Journal, Alternate-Self, and Imagination initialization.
- **Finding:** if creating the Hands worker or autonomous-thinking thread fails, the failure paths stopped/joined what they needed but only shut down the diary and closed the main memory database before returning `-1`. Other initialized subsystem handles/state could remain live. The normal `main()` path returned immediately when `r2_init()` failed instead of calling `r2_shutdown()`.
- **Fix:** both startup thread-creation failures now return through one cleanup owner: `main()` calls `r2_shutdown()` when initialization fails. The autonomous-thinking creation failure still stops and joins the already-started Hands worker before returning; central shutdown then closes Reality, Eyes/Ears, Visual, Reward, Life Log, Sensory Journal, diary, memory DB, and other initialized runtime state in normal shutdown order. Production behavior still uses `pthread_create`; deterministic environment-driven failure injection is compiled only with `R2_TEST_STARTUP_FAILURE_INJECTION`.
- **Regression coverage:** `tests/startup_failure_unwind_smoke.py` verifies both injected failure points route through centralized cleanup and that the production build cannot activate the test hooks. The workflow runs this regression and compiles R2 with the launcher-equivalent sources/flags. This source-level regression does not substitute for a target-host fault-injection run.
- **Status:** fixed and validated on source commit `908f05aa7850e2cd479f02e2a391886efbcc2c33`. Both complete workflows passed (87/87 steps each): push [39192095002](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39192095002) and pull request [39192098821](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39192098821). The current audit-log update is documentation-only; the resulting exact final head is being revalidated. Target-host fault-injection remains unverified.

## Manual source-review checkpoint: conversation and memory pipeline

Reviewed the current implementations and call flow in `r2.c`, `r2.h`, `R2Reply.c`, `Visual.c`, and the production-backed conversation regression tests.

- The foreground conversation lock spans the whole turn, preventing interleaved user/assistant turns and failed-turn rollback from removing another caller's newer messages.
- The active user message is preserved as a real user-role message; retrieval is assembled separately as historical-evidence system context. The payload builder reserves the authoritative user turn and refuses inference if that turn cannot be serialized in full.
- Retrieved memories and Life Log/diary/visual evidence are bounded before prompt insertion. The short-lived cache stores only generic persistent-memory retrieval; current Reality and live visual observations are refreshed separately.
- The shared Ollama gate gives foreground text inference priority over opportunistic background work and vision requests. Tool results are tagged as internal and are not confused with a fresh human user turn.
- The reply sanitizer removes private reasoning blocks, fails closed for unclosed private blocks, and preserves/redacts tag syntax in code examples. Its tests cover nested/crossing tags and malformed truncation.
- The post-turn bookkeeping is scheduled after user-facing delivery; worker-start failure is logged instead of blocking the response.

## Manual source-review checkpoint: tool routing, filesystem tools, and persistence

- Traced all three background command dispatch paths through `start_background_task()`; fixed false-success reporting and added a regression.
- Traced the Game Boy control-plane response contract against `GameBoyAdvance.py`; fixed fail-open JSON handling and added a regression.
- Reviewed filesystem tool path resolution: workspace paths are canonicalized and checked for workspace containment; fenced/HTML code examples are excluded by the control-marker parser. This pass did not establish a reproducible path-escape defect, though filesystem writes are not a full descriptor-relative defense against a hostile concurrent symlink swap.
- Traced Reality add/move/remove paths and the existing money/purchase journals. The separate fridge/pockets crash window was fixed under R2-AUD-012 with a durable snapshot journal and restart-injection tests.
- Continued the startup/shutdown review and fixed direct `r2_init()` fatal exits so required initialization failures route through centralized cleanup, with state-aware libcurl and diary cleanup.

This remains a bounded manual review, not a claim that every subsystem has been audited. The latest-head full workflow passed on commit `7ba1cdac72d0e244f19db1de831b398ed4f40dbc` (90/90 steps; run [39197843416](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39197843416)). Target-host acceptance remains open.

## Release gates still open

- Full workflow passed on code/test commit `cc375fe440407ef87b7576e2fe7a4840994c6218` (90/90 steps; run [39198701049](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39198701049)). It includes the tool-result allocation-failure warning contract and direct no-retry response. This audit-log update is documentation-only; the target-machine acceptance gates below remain open.
- On the target machine, verify real Qwen 3.5 9B multi-turn chat, retry/fallback behavior, measured CPU/GPU placement and RAM headroom, and connected devices. CI cannot perform these checks.
- Before installation, verify that the trusted root bootstrap is installed at `/usr/local/sbin/r2-3po-install`, root-owned, and not group/world-writable. The documented immutable source commit and Git blob SHA are checked against the current branch; this repository check does not prove the host has installed that copy.
- Keep PR #161 draft and `main` untouched until repository validation and target-host acceptance are complete.

### R2-AUD-009 — Fatal exits inside `r2_init()` bypass the centralized unwind path

- **Area:** `r2.c`, early and mid-stage failures inside `r2_init()`.
- **Finding:** multiple initialization failures still call the process-wide `die()` helper, which calls `exit(EXIT_FAILURE)`. This bypasses `main()`'s new `r2_init() != 0` cleanup path. Several of these branches perform hand-written partial cleanup first, making a blind conversion to `return -1` unsafe: the shutdown guard currently checks only `core_initialized`, `db`, `eyes`, and `ears`, so it can skip cleanup after a branch has already nulled those pointers while other subsystems or sound state may still be initialized.
- **Impact:** OS process teardown will close ordinary process-owned descriptors, but R2's own orderly subsystem shutdown, consistent failure reporting, and in-process retry/unwind contract are not guaranteed for these startup failures.
- **Fix:** required failures in `r2_init()` now report an error and return to `main()`'s centralized cleanup owner instead of calling process-wide `exit()`. An explicit init-attempt flag makes cleanup run even if an earlier branch already nulled the core handles; libcurl cleanup is gated on successful initialization; the diary handle is closed when its initializer fails after opening it. Unrelated runtime `die()` behavior is unchanged.
- **Regression coverage:** expanded `tests/startup_failure_unwind_smoke.py` to check direct init exits are gone, early partial initialization reaches central cleanup, and libcurl/diary cleanup are state-aware. Existing thread-creation failure checks remain.
- **Status:** fixed and validated on commit `d9c1ff5e3d4f4d48f735ee03374260fde3d0e503`; full workflow [39194830081](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39194830081) passed (89/89 steps). The smoke test checks source contracts and existing thread-failure regressions; it does not inject every initialization failure at runtime.

### R2-AUD-010 — Background command dispatch could report success when no task was queued

- **Area:** `r2.c`, structured `[HAND_REQUEST]`, explicit `[RUN]`, and direct `[HAND]` tool dispatch.
- **Finding:** `start_background_task()` returns `NULL` when allocation fails or shutdown has already begun. All three callers previously printed “Background task successfully started” anyway, with an empty task ID, which could tell R2 and the user that a command was running when no work had entered the queue.
- **Fix:** each caller now branches on the returned task ID and emits an explicit enqueue-failure result when the queue operation fails. It does not claim execution or invent a task ID.
- **Regression coverage:** added `tests/hand_enqueue_failure_smoke.py` and wired it into `.github/workflows/build.yml` to check all three dispatch sites.
- **Status:** fixed and validated on commit `d9c1ff5e3d4f4d48f735ee03374260fde3d0e503`; the regression passed and the full workflow [39194830081](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39194830081) passed (89/89 steps). No target-machine execution is implied.

### R2-AUD-011 — Game Boy command output could be accepted without a valid success response

- **Area:** `r2.c`, Game Boy Advance command bridge and JSON response parsing.
- **Finding:** the bridge defaulted a missing or unparsable `ok` field to true. Because `gameboy_json_bool()` also accepted non-boolean JSON values, a zero exit code with malformed output or an invalid `ok` value could be treated as a successful action. That could produce a false success response or record an action that the bridge had not verified.
- **Fix:** JSON boolean fields now require the actual JSON boolean type. The command bridge accepts the documented array shape for `list`, the object shape for `status`, and requires an explicit boolean `ok: true` for action responses; process exit code alone is insufficient.
- **Regression coverage:** added `tests/gameboy_json_contract_smoke.py` and wired it into `.github/workflows/build.yml`. This is a source-contract regression, not a live emulator/hardware test.
- **Status:** fixed and validated on commit `d9c1ff5e3d4f4d48f735ee03374260fde3d0e503`; the response-contract regression, strict build, and full workflow [39194830081](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39194830081) passed (89/89 steps). This is not a live emulator/hardware test.

### R2-AUD-012 — Cross-database fridge transfers lacked crash recovery

- **Area:** `Reality.c`, moving a tracked stack into the separate fridge database and taking food from the fridge into pockets.
- **Finding:** both directions updated the fridge database and Reality database in separate operations with only in-process rollback. A process crash between writes could duplicate a stored item or lose a taken food item.
- **Fix:** added backward-compatible `r2_inventory_transfer_journal` in the Reality database. Before either side changes, it persists a snapshot of the prior Reality object, item-memory high-water mark, and fridge row metadata/quantity. Meal transfers also snapshot modeled needs, the 72-hour starvation marker, and food-experience high-water mark. Both transfer directions mark the journal committed only after both database changes succeed. Startup and subsequent transfer/purchase paths recover a pending journal by restoring both databases and mirrors; committed journals are cleared without undoing a completed transfer. Meal entry recovers any retained journal before advancing the clock, preventing a later rollback from erasing newly elapsed time. Interrupted recovery is recorded in the factual log.
- **Regression coverage:** extended `tests/reality_needs_smoke.c` with test-only crash injection after the fridge write in each direction. The test restarts Reality and checks both authoritative databases, prior fridge metadata, and journal clearance.
- **Validation:** latest-head workflow [39197843416](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39197843416) passed all 90/90 steps. This includes strict production compilation, the Reality needs smoke test with crash-injected fridge-to-pocket, room-to-fridge, and fridge-meal recovery, startup-thread failure cleanup, and isolated overnight SIGTERM lifecycle integration. CI still does not prove behavior on the user's physical machine.



### R2-AUD-013 — Tool-result allocation failure could hide a completed side effect

- **Area:** `r2.c`, `process_tools_with_status()` result-buffer growth.
- **Finding:** the `APPEND` macro returned `NULL` if `realloc(results, cap)` failed. That allocation can happen after a tool has already changed persistent state. The caller would then receive a generic tool-processing failure instead of a warning that an action may have completed, making a manual retry risky.
- **Fix:** on result-buffer growth failure, preserve the existing allocation and return an explicit warning that processing had begun, an action may already have completed, and R2 will not repeat it automatically. Returning from the tool processor also stops subsequent tool markers from executing.
- **Regression coverage:** extended `tests/conversation_recovery_smoke.py` to check this allocation-failure contract. This is a source-contract regression; the failure is not injected at runtime.
- **Status:** fixed and validated on code/test commit `cc375fe440407ef87b7576e2fe7a4840994c6218`; full workflow [39198701049](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39198701049) passed all 90/90 steps, including the updated conversation-recovery regression and strict production build. The allocation-failure branch is protected by a source-contract test, not an injected runtime OOM test.

### R2-AUD-014 — Ollama readiness and vision paths accepted incomplete model responses

- **Area:** `tools/preflight_ollama_model.py` text/image readiness checks; `Visual.c` live visual inference response parsing.
- **Finding:** the core conversation path requires an Ollama response to contain `message.role=assistant` and boolean `done=true`, but the startup text/vision preflight and live Visual subsystem accepted non-empty `message.content` without checking that completion contract. A partial or malformed response could therefore be treated as a successful readiness probe or visual description.
- **Fix:** preflight now rejects text and image replies unless the assistant role and explicit boolean completion flag are present. Visual inference applies the same contract and logs `vision_invalid_completion_contract` when it rejects a malformed response.
- **Regression coverage:** extended `tests/ollama_preflight_smoke.py` with incomplete text/image response cases and extended `tests/vision_watch_cancellation_smoke.py` to guard the live vision contract.
- **Status:** fixed and validated on the release candidate tested by GitHub Actions run [4847](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39204350398), which passed all 90/90 steps, including the offline Ollama preflight regression. This does not replace target-machine model or camera testing.

### R2-AUD-015 — Control-only model replies could pass the visible-answer gate

- **Area:** `r2.c`, `reply_has_visible_text()` before `process_tools()`.
- **Finding:** the visibility predicate only checked for non-whitespace bytes. A reply containing only a command block such as `[RUN]... [END RUN]` or a one-line action marker such as `[WORLD]` was therefore treated as a visible answer, despite the surrounding recovery logic intending to retry marker-only output before any action executes.
- **Fix:** the visibility check now ignores recognized action-control blocks and command lines, and fails closed on an unterminated control block. A reply must contain actual non-control text before tool processing is allowed.
- **Regression coverage:** extended `tests/conversation_recovery_smoke.py` to guard the control-marker filtering and its placement before action execution, and added runtime assertions to `tests/recent_context_budget_runtime.c` for marker-only, malformed, and ordinary replies.
- **Status:** fixed and validated on the release candidate tested by GitHub Actions run [4847](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39204350398), which passed all 90/90 steps, including the runtime reply-visibility checks and strict production build. No target-machine execution is implied.

### R2-AUD-016 — Tool-result OOM warning could overrun the original allocation

- **Area:** `r2.c`, `process_tools_with_status()`, tool-result buffer growth failure.
- **Finding:** the growth loop doubled `cap` in place before calling `realloc()`. If `realloc()` failed, the existing `results` allocation still had its original size, but the failure warning used the already-increased `cap` as the `snprintf()` bound. Under memory pressure, this could write beyond the original allocation.
- **Fix:** compute the desired capacity in a separate `_new_cap` variable; leave `cap` unchanged until `realloc()` succeeds. The warning is now formatted with the actual original allocation size. Capacity doubling also checks against `SIZE_MAX` before multiplying.
- **Regression coverage:** updated `tests/conversation_recovery_smoke.py` to assert the failure path formats the warning using the original `cap`, does not commit `_new_cap` before successful reallocation, and updates `cap` only after adopting the new buffer.
- **Status:** fix and regression committed on the release branch. CI for this commit is pending; no claim of target-machine execution.
