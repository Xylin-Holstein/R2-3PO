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
- **Finding:** the function validates current bill/change entries, then unlinks all existing authoritative bill files and `change.txt` before creating the requested replacement bill files. A failure during replacement (for example, a later file creation/write failure after earlier new bills were created) returns an error after the prior authoritative balance has already been destroyed. Callers attempt rollback by recreating the old balance, but that rollback can fail for the same underlying filesystem condition. The current “preflight” only catches malformed existing entries; it does not make the multi-file update transactional.
- **Impact:** a disk/write failure during a wallet or piggybank mutation can leave the physical money files at a partial or lost balance, despite the operation reporting failure. This conflicts with the explicit rule that physical files are authoritative and can affect purchases and transfers.
- **Fix in this branch:** replacement bills/change are staged before authoritative files move. A phase marker and same-filesystem backup let startup recovery distinguish staging-only, partial backup movement, partial installation, and committed replacement. Wallet and piggybank recovery runs before balances are read at startup; synchronous failures use the same recovery path.
- **Regression coverage added:** test-only compile-time failure injection forces staging to fail and checks the original $5 balance is unchanged. A restart fixture simulates a crash during installation and checks recovery restores the original bill set.
- **Status:** implementation and regressions passed both full GitHub Actions workflows on code commit `82e14b3e07460bf8b15778d2ac63dd2b2355ab59` (87/87 steps in each; push run [39185665132](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39185665132), pull-request run [39185660924](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39185660924)). Target-host power-loss and filesystem behavior remain unverified.

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

- Money transaction source and regression tests passed both full workflows on `82e14b3e07460bf8b15778d2ac63dd2b2355ab59` (87/87 steps each). The later audit-log-only commit records that result; it does not change runtime source.
- On the target machine, verify real Qwen 3.5 9B multi-turn chat, retry/fallback behavior, measured CPU/GPU placement and RAM headroom, and connected devices. CI cannot perform these checks.
- Before installation, verify that the trusted root bootstrap is installed at `/usr/local/sbin/r2-3po-install`, root-owned, and not group/world-writable. The documented immutable source commit and Git blob SHA are checked against the current branch; this repository check does not prove the host has installed that copy.
- Keep PR #161 draft and `main` untouched until repository validation and target-host acceptance are complete.
