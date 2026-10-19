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

No additional reproducible defect was established in this review slice. This is a bounded checkpoint, not a claim that every subsystem has now been audited. Next source-review batch should continue into tool routing/side effects and Life Log/Reality database writes, then cover shutdown and service lifecycle paths.

## Release gates still open

- Money replacement, recovery, and transfer journal source/tests passed both full workflows on code commit `9fe05d0e000243b4b1ba9b515c39f68259e287bd` (87/87 steps each). The later audit-log-only commit records those results but does not change runtime source.
- On the target machine, verify real Qwen 3.5 9B multi-turn chat, retry/fallback behavior, measured CPU/GPU placement and RAM headroom, and connected devices. CI cannot perform these checks.
- Before installation, verify that the trusted root bootstrap is installed at `/usr/local/sbin/r2-3po-install`, root-owned, and not group/world-writable. The documented immutable source commit and Git blob SHA are checked against the current branch; this repository check does not prove the host has installed that copy.
- Keep PR #161 draft and `main` untouched until repository validation and target-host acceptance are complete.

### R2-AUD-009 — Fatal exits inside `r2_init()` bypass the centralized unwind path

- **Area:** `r2.c`, early and mid-stage failures inside `r2_init()`.
- **Finding:** multiple initialization failures still call the process-wide `die()` helper, which calls `exit(EXIT_FAILURE)`. This bypasses `main()`'s new `r2_init() != 0` cleanup path. Several of these branches perform hand-written partial cleanup first, making a blind conversion to `return -1` unsafe: the shutdown guard currently checks only `core_initialized`, `db`, `eyes`, and `ears`, so it can skip cleanup after a branch has already nulled those pointers while other subsystems or sound state may still be initialized.
- **Impact:** OS process teardown will close ordinary process-owned descriptors, but R2's own orderly subsystem shutdown, consistent failure reporting, and in-process retry/unwind contract are not guaranteed for these startup failures.
- **Status:** confirmed by manual source tracing; open. Do not replace `die()` calls mechanically. Fix requires explicit per-subsystem initialization state (including libcurl and sound lifecycle), one idempotent unwind path, and injected failures at each required-init boundary; preserve fatal behavior for unrelated runtime helper calls.

### R2-AUD-010 — Background command dispatch could report success when no task was queued

- **Area:** `r2.c`, structured `[HAND_REQUEST]`, explicit `[RUN]`, and direct `[HAND]` tool dispatch.
- **Finding:** `start_background_task()` returns `NULL` when allocation fails or shutdown has already begun. All three callers previously printed “Background task successfully started” anyway, with an empty task ID, which could tell R2 and the user that a command was running when no work had entered the queue.
- **Fix:** each caller now branches on the returned task ID and emits an explicit enqueue-failure result when the queue operation fails. It does not claim execution or invent a task ID.
- **Regression coverage:** added `tests/hand_enqueue_failure_smoke.py` and wired it into `.github/workflows/build.yml` to check all three dispatch sites.
- **Status:** source fix and regression wiring committed on the release branch. The regression and full build workflow still need to pass on the resulting head; no target-machine execution is implied.

### R2-AUD-011 — Game Boy command output could be accepted without a valid success response

- **Area:** `r2.c`, Game Boy Advance command bridge and JSON response parsing.
- **Finding:** the bridge defaulted a missing or unparsable `ok` field to true. Because `gameboy_json_bool()` also accepted non-boolean JSON values, a zero exit code with malformed output or an invalid `ok` value could be treated as a successful action. That could produce a false success response or record an action that the bridge had not verified.
- **Fix:** JSON boolean fields now require the actual JSON boolean type. The command bridge accepts the documented array shape for `list`, the object shape for `status`, and requires an explicit boolean `ok: true` for action responses; process exit code alone is insufficient.
- **Regression coverage:** added `tests/gameboy_json_contract_smoke.py` and wired it into `.github/workflows/build.yml`. This is a source-contract regression, not a live emulator/hardware test.
- **Status:** source fix and regression wiring committed on the release branch. The current branch head still needs the full workflow result.

