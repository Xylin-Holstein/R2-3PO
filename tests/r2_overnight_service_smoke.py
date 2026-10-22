#!/usr/bin/env python3
"""Static safety/regression checks for R2's unattended overnight service."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OvernightServiceTests(unittest.TestCase):
    def test_fixed_private_clock_offset_rolls_over_without_rebasing(self):
        from datetime import datetime, timezone

        offset = -852076800
        baseline = datetime(2026, 10, 13, 12, 0, 0, tzinfo=timezone.utc)
        baseline_virtual = datetime.fromtimestamp(baseline.timestamp() + offset, timezone.utc)
        self.assertEqual(
            baseline_virtual,
            datetime(1999, 10, 13, 12, 0, 0, tzinfo=timezone.utc),
        )

        before = datetime(1999, 12, 31, 23, 59, 59, tzinfo=timezone.utc)
        host_before = before.timestamp() - offset
        virtual_before = datetime.fromtimestamp(host_before + offset, timezone.utc)
        virtual_after = datetime.fromtimestamp(host_before + 1 + offset, timezone.utc)
        self.assertEqual(virtual_before, before)
        self.assertEqual(virtual_after, datetime(2000, 1, 1, 0, 0, 0, tzinfo=timezone.utc))

    def test_shell_has_signal_driven_noninteractive_mode(self):
        source = (ROOT / "shell.c").read_text(encoding="utf-8")
        self.assertIn('getenv("R2_SERVICE_MODE")', source)
        self.assertIn('!strcmp(service_mode_value, "1")', source)
        self.assertIn("if (service_mode) {", source)
        self.assertIn("sigsuspend(&wait_mask)", source)
        self.assertNotIn("pause();", source)
        self.assertIn("sigprocmask(SIG_BLOCK, &service_signals, &original_mask)", source)
        self.assertIn("SIGTERM", source)
        self.assertIn("static atomic_int shell_shutdown", source)
        self.assertIn("atomic_is_lock_free(&shell_shutdown)", source)
        self.assertIn("static atomic_int tv_control_stop", source)
        self.assertIn("int server_fd = tv_control_fd", source)
        self.assertIn("accept(server_fd", source)
        core = (ROOT / "r2.c").read_text(encoding="utf-8")
        self.assertIn("r2_shutdown();", core)
        self.assertIn("[R2 READY] Core initialized; entering shell.", core)
        self.assertIn("static atomic_int shutting_down", core)
        self.assertIn("atomic_is_lock_free(&shutting_down)", core)
        self.assertIn("setvbuf(stdout, NULL, _IOLBF, 0)", core)
        self.assertIn("sigprocmask(SIG_BLOCK, &service_signals, NULL)", core)
        self.assertNotIn('safely.\\\\n");', core)
        self.assertLess(core.index("sigprocmask(SIG_BLOCK, &service_signals, NULL)"),
                        core.index("if (r2_init() != 0)"))

    def test_runner_checks_local_model_and_private_clock(self):
        source = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
        self.assertIn("qwen3.5:9b", source)
        self.assertIn("refusing symlinked R2 database", source)
        self.assertIn("r2_fridge.db", source)
        self.assertIn("r2_rewards.db", source)
        self.assertIn("r2_addictions.db", source)
        self.assertIn("for helper in ffmpeg ffprobe pactl xdotool", source)
        self.assertIn("no local MP3 player", source)
        self.assertIn("VLC or python3-vlc is unavailable", source)
        self.assertIn("Tkinter is unavailable", source)
        self.assertIn("preflight_ollama_model.py", source)
        self.assertIn('"R2_OLLAMA_BASE_URL=$R2_OLLAMA_BASE_URL"', source)
        probe = (ROOT / "tools/preflight_ollama_model.py").read_text(encoding="utf-8")
        self.assertIn('os.environ.get("R2_OLLAMA_BASE_URL", DEFAULT_API_BASE)', probe)
        self.assertIn('f"{api_base}/api/tags"', probe)
        self.assertIn('f"{api_base}/api/chat"', probe)
        self.assertIn('f"{api_base}/api/ps"', probe)
        self.assertIn("r2_ollama_chat_url(ollama_url, sizeof(ollama_url))", (ROOT / "r2.c").read_text(encoding="utf-8"))
        self.assertIn("r2_ollama_chat_url(ollama_url, sizeof(ollama_url))", (ROOT / "Visual.c").read_text(encoding="utf-8"))
        self.assertIn("export R2_OLLAMA_BASE_URL", source)
        self.assertIn("--wait-seconds 60 --timeout 180", source)
        self.assertIn("real chat completion", source)
        self.assertIn("LD_PRELOAD", source)
        self.assertIn("FAKETIME_DONT_RESET=1", source)
        self.assertIn("R2_SERVICE_MODE=1", source)
        self.assertIn('"R2_OLLAMA_TIMEOUT_SECONDS=${R2_OLLAMA_TIMEOUT_SECONDS:-}"', source)
        self.assertIn('export HOME="$R2_ROOT"', source)
        self.assertIn('"HOME=$R2_ROOT"', source)
        self.assertIn("systemd-inhibit --what=sleep", source)
        self.assertLess(source.index("exec systemd-inhibit"), source.index('log "Waiting for local Qwen 3.5 model availability and a real chat completion..."'))
        self.assertIn("FAKETIME_OFFSET=-852076800", source)
        self.assertIn("R2_EPOCH=$((REAL_EPOCH + FAKETIME_OFFSET))", source)
        self.assertNotIn('R2_START="1999-${CURRENT_MONTH}', source)
        self.assertNotIn("FAKETIME_OFFSET=$((R2_EPOCH - REAL_EPOCH))", source)
        self.assertNotIn("apt-get update", source)
        self.assertNotIn("ollama pull", source)

    def test_unit_restarts_failures_and_runs_as_dedicated_user(self):
        source = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
        self.assertIn("User=r2", source)
        self.assertIn("SupplementaryGroups=audio video render", source)
        self.assertIn("Environment=HOME=/home/x/R2_Home", source)
        self.assertIn("R2_SVC_NETLOCK", source)
        self.assertIn("--uid-owner r2", source)
        self.assertIn("--dport 11435", source)
        self.assertIn("ip6tables", source)
        self.assertIn("ExecStopPost=", source)
        self.assertIn("Restart=always", source)
        self.assertIn("RestartSec=10", source)
        self.assertIn("TimeoutStopSec=240", source)
        self.assertIn("WantedBy=multi-user.target", source)
        ollama_unit = (ROOT / "deploy/r2-ollama.service").read_text(encoding="utf-8")
        self.assertIn("User=r2", ollama_unit)
        self.assertIn("OLLAMA_HOST=127.0.0.1:11435", ollama_unit)
        self.assertIn("OLLAMA_MODELS=/home/x/R2_Home/.ollama/models", ollama_unit)
        self.assertIn("R2_OLLAMA_NETLOCK", ollama_unit)
        self.assertIn("--dport 11435", ollama_unit)
        self.assertIn("--dport 11434", ollama_unit)  # interactive launcher stays supported
        self.assertIn("R2_OLLAMA_NETLOCK -j REJECT", ollama_unit)
        ollama_runner = (ROOT / "tools/r2_ollama_runner.sh").read_text(encoding="utf-8")
        self.assertIn("OLLAMA_HOST", ollama_runner)
        self.assertIn('exec "$OLLAMA_BIN" serve', ollama_runner)
        self.assertIn("R2_SERVICE_MODE=1", source)
        self.assertIn("r2-ollama.service", source)
        self.assertIn("Environment=R2_OLLAMA_BASE_URL=http://127.0.0.1:11435", source)
        self.assertIn("Environment=R2_MODEL=qwen3.5:9b", source)
        # GPU layer policy is selected by the runner from actual hardware or
        # an explicit override, not hard-coded in the systemd unit.
        self.assertNotIn("Environment=R2_OLLAMA_NUM_GPU=", source)
        self.assertIn("GTX 1050 Ti", (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8"))
        self.assertIn("R2_OLLAMA_NUM_GPU=8", (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8"))
        launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
        self.assertIn("Checking the local Ollama API and required model", launcher)
        self.assertIn('TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"', launcher)
        self.assertIn('if [ -e "$TEMP_EXEC" ] || [ -L "$TEMP_EXEC" ]; then', launcher)
        self.assertIn("preflight_ollama_model.py", launcher)
        self.assertIn("qwen3.5:9b", launcher)
        self.assertLess(launcher.index("Checking the local Ollama API"), launcher.index('echo "[3/4] Recompiling R2..."'))
        self.assertIn("systemctl is-active --quiet r2-3po.service", launcher)
        self.assertIn("two R2 cores", launcher)
        self.assertIn("pgrep -x r2", launcher)
        self.assertIn("R2_3PO_UID_NETLOCK", launcher)
        self.assertIn('--uid-owner "$R2_UID"', launcher)
        self.assertIn("--dport 11434", launcher)
        self.assertIn('HOME="$R2_ROOT"', launcher)
        self.assertIn("An R2 core is already running", launcher)
        self.assertIn('REAL_EPOCH="$(date \'+%s\')"' , launcher)
        self.assertIn("FAKETIME_OFFSET=-852076800", launcher)
        self.assertIn("R2_EPOCH=$((REAL_EPOCH + FAKETIME_OFFSET))", launcher)
        self.assertNotIn('R2_START="1999-${CURRENT_MONTH}', launcher)
        self.assertNotIn("FAKETIME_OFFSET=$((R2_EPOCH - REAL_EPOCH))", launcher)
        self.assertIn("no relaunch reset", launcher)
        self.assertIn('R2_OLLAMA_TIMEOUT_SECONDS="${R2_OLLAMA_TIMEOUT_SECONDS:-}"', launcher)
        self.assertIn('sudo -n -u r2 -- gcc -std=c11 -Wall -Wextra -Werror -O2', launcher)
        self.assertIn('if [[ -z "${R2_OLLAMA_NUM_GPU:-}" ]]; then', launcher)
        self.assertIn("GTX 1050 Ti", launcher)
        self.assertIn("export R2_OLLAMA_NUM_GPU=8", launcher)
        self.assertIn('R2_OLLAMA_NUM_GPU="${R2_OLLAMA_NUM_GPU}"', launcher)
        self.assertIn("R2_RUNTIME_GROUPS=", launcher)
        self.assertIn('R2_RUNTIME_GROUPS+=",${DEVICE_GID}"', launcher)
        self.assertIn("for DEVICE_GROUP in audio video; do", launcher)
        self.assertIn('sudo -n setpriv --reuid="$R2_UID" --regid="$R2_GID" --groups="$R2_RUNTIME_GROUPS" --', launcher)
        self.assertNotIn('sudo -u r2 -g r2 -G "$R2_RUNTIME_GROUPS"', launcher)
        self.assertNotIn("usermod -aG audio", launcher)

    def test_installer_is_non_destructive_and_does_not_update_apt(self):
        source = (ROOT / "tools/r2_install_root_bootstrap.sh").read_text(encoding="utf-8")
        wrapper = (ROOT / "tools/install_r2_overnight.sh").read_text(encoding="utf-8")
        self.assertIn("r2_rewards.db", source)
        self.assertIn("r2_addictions.db", source)
        self.assertIn('TEMP_EXEC="${R2_ROOT}/r2.new.${BASHPID}"', source)
        self.assertIn('if [[ -e "$TEMP_EXEC" || -L "$TEMP_EXEC" ]]; then', source)
        self.assertIn('mv -f "$TEMP_EXEC" "$R2_EXEC"', source)
        self.assertIn('BACKUP_EXEC="$(run_as_r2 mktemp "$R2_ROOT/r2.previous.XXXXXX")"', source)
        self.assertIn('cp "$R2_EXEC" "$BACKUP_EXEC"', source)
        self.assertIn("rollback_previous_executable() {", source)
        self.assertIn('run_as_r2 cp "$BACKUP_EXEC" "$restore_exec"', source)
        self.assertIn("NEW_EXEC_INSTALLED=1", source)
        self.assertIn("automatic early-exit rollback", source)
        self.assertIn("trap cleanup_on_exit EXIT", source)
        self.assertIn('run_as_r2 chmod 0755 "$R2_EXEC"', source)
        self.assertIn("refusing symlinked existing executable", source)
        self.assertEqual(source.count("rollback_previous_executable || log"), 2)
        self.assertLess(source.index('cp "$R2_EXEC" "$BACKUP_EXEC"'),
                        source.index('mv -f "$TEMP_EXEC" "$R2_EXEC"'))
        self.assertLess(
            source.index('python3 "$R2_SOURCE/tools/preflight_ollama_model.py" --wait-seconds 60'),
            source.index('BACKUP_EXEC="$(run_as_r2 mktemp "$R2_ROOT/r2.previous.XXXXXX")"'),
        )
        self.assertIn("systemctl enable --now r2-3po.service", source)
        self.assertIn("copy_ollama_model.py", source)
        self.assertIn("R2_OLLAMA_UNIT_EOF", source)
        self.assertIn("R2_SERVICE_UNIT_EOF", source)
        self.assertIn("do not execute this mutable source-tree script as root", wrapper)
        self.assertNotIn("root_run ", wrapper)
        # The privileged bootstrap's embedded units are the exact definitions
        # installed by root; they must stay synchronized with reviewed repo units.
        ollama_unit = (ROOT / "deploy/r2-ollama.service").read_text(encoding="utf-8").rstrip("\n")
        r2_unit = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8").rstrip("\n")
        ollama_marker = 'cat > "$UNIT_STAGING_DIR/r2-ollama.service" <<\'R2_OLLAMA_UNIT_EOF\'\n'
        r2_marker = 'cat > "$UNIT_STAGING_DIR/r2-3po.service" <<\'R2_SERVICE_UNIT_EOF\'\n'
        ollama_start = source.index(ollama_marker) + len(ollama_marker)
        r2_start = source.index(r2_marker) + len(r2_marker)
        ollama_end = source.index("\nR2_OLLAMA_UNIT_EOF", ollama_start)
        r2_end = source.index("\nR2_SERVICE_UNIT_EOF", r2_start)
        self.assertEqual(source[ollama_start:ollama_end], ollama_unit)
        self.assertEqual(source[r2_start:r2_end], r2_unit)
        self.assertIn("systemctl enable --now r2-ollama.service", source)
        self.assertIn("preflight_ollama_model.py", source)
        self.assertIn("PREFLIGHT_GPU=8", source)
        self.assertIn("PREFLIGHT_CTX=\"\"", source)
        self.assertIn('R2_OLLAMA_NUM_GPU="$PREFLIGHT_GPU" R2_OLLAMA_NUM_CTX="$PREFLIGHT_CTX"', source)
        self.assertIn("4 GPU layers and 8192 context tokens", source)
        self.assertNotIn("ollama pull", source)
        self.assertNotIn("apt-get update", source)
        self.assertIn("pgrep -x r2", source)
        self.assertIn("FAKETIME_FOUND", source)
        self.assertIn("refusing symlinked R2 database", source)
        self.assertIn("audit-snapshot-", source)
        self.assertIn("integrity audit failed", source)
        self.assertIn('systemctl is-active --quiet r2-3po.service', source)
        self.assertIn("did not reach active state", source)
        self.assertIn('local restore_exec="${R2_ROOT}/r2.restore.${BASHPID}"', source)
        self.assertIn("wait_for_core_ready()", source)
        self.assertIn('grep -Fq "[R2 READY] Core initialized; entering shell."', source)
        self.assertNotIn('grep -Fq "Starting R2 with private clock"', source)


    def test_installer_restores_private_ollama_state_after_early_failure(self):
        installer = (ROOT / "tools/r2_install_root_bootstrap.sh").read_text(encoding="utf-8")
        self.assertIn("OLLAMA_WAS_ACTIVE=0", installer)
        self.assertIn("OLLAMA_WAS_ENABLED=0", installer)
        self.assertIn(
            'OLLAMA_ACTIVE_STATE="$(root_run systemctl is-active r2-ollama.service 2>/dev/null || true)"',
            installer,
        )
        self.assertIn(
            'OLLAMA_ENABLED_STATE="$(root_run systemctl is-enabled r2-ollama.service 2>/dev/null || true)"',
            installer,
        )
        self.assertIn("active) OLLAMA_WAS_ACTIVE=1 ;;", installer)
        self.assertIn("enabled|enabled-runtime|linked|linked-runtime) OLLAMA_WAS_ENABLED=1 ;;", installer)
        self.assertIn("OLLAMA_STATE_CAPTURED=1", installer)
        self.assertIn('if [[ "$INSTALL_SUCCEEDED" != "1"', installer)
        self.assertIn('R2_ROLLBACK_RESTARTED=1', installer)
        self.assertIn('INSTALL_SUCCEEDED=1', installer)
        self.assertIn('run_as_r2 rm -f -- "$R2_EXEC"', installer)
        self.assertIn('root_run systemctl disable r2-3po.service >/dev/null 2>&1 || true', installer)
        self.assertIn('NEW_EXEC_INSTALLED=0', installer)
        model_copy_section = installer.index("# Keep the desktop user's Ollama service untouched.")
        stop_at = installer.index(
            'root_run systemctl stop r2-ollama.service >/dev/null 2>&1 || true',
            model_copy_section,
        )
        self.assertLess(installer.index("OLLAMA_STATE_CAPTURED=1"), stop_at)
        state_check_at = installer.index(
            'OLLAMA_UNIT_STATE="$(root_run systemctl is-active r2-ollama.service',
            stop_at,
        )
        model_copy_at = installer.index('run_as_r2 python3 "$MODEL_COPY_SCRIPT"', model_copy_section)
        self.assertLess(stop_at, state_check_at)
        self.assertLess(state_check_at, model_copy_at)
        self.assertIn("pgrep -u r2 -x ollama", installer)
        self.assertIn("OLLAMA_ACTIVE_STATE=", installer)
        self.assertIn("OLLAMA_ENABLED_STATE=", installer)
        self.assertIn("could not verify that the private Ollama service stopped", installer)
        self.assertIn("could not verify that no r2-owned Ollama process remains", installer)

if __name__ == "__main__":
    unittest.main()
