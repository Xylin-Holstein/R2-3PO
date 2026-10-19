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
- **Status:** confirmed by source tracing; not fixed in this batch. Next batch should make the purchase intent recoverable across physical money and the item database, with regression cases for interruption before and after the inventory commit. Do not describe purchase crash consistency as solved.


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
