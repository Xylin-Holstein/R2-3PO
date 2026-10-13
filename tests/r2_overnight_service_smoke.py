#!/usr/bin/env python3
"""Static safety checks for R2's split core/inference systemd services."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class OvernightServiceTests(unittest.TestCase):
    def test_shell_stays_alive_until_a_service_stop_signal(self):
        source = (ROOT / "shell.c").read_text(encoding="utf-8")
        self.assertIn('getenv("R2_SERVICE_MODE")', source)
        self.assertIn('!strcmp(service_mode_value, "1")', source)
        self.assertIn("shell_wait_for_service_signal()", source)
        self.assertIn("sigsuspend(&wait_mask)", source)
        self.assertIn("SIGTERM", source)
        self.assertIn("SIGINT", source)
        self.assertIn("sigprocmask(SIG_BLOCK, &service_signals, &original_mask)", source)
        self.assertNotIn("pause();", source)

    def test_runner_waits_for_a_real_private_gemma_completion(self):
        source = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
        self.assertIn("gemma3:4b", source)
        self.assertIn("http://127.0.0.1:11435", source)
        self.assertIn("preflight_ollama_model.py", source)
        self.assertIn("--wait-seconds 60 --timeout 180", source)
        self.assertIn("real chat completion", source)
        self.assertIn("R2_SERVICE_MODE=1", source)
        self.assertIn('export HOME="$R2_ROOT"', source)
        self.assertNotIn("ollama pull", source)
        self.assertNotIn("apt-get update", source)

    def test_core_service_binds_to_isolated_r2_owned_model_service(self):
        source = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
        self.assertIn("User=r2", source)
        self.assertIn("Environment=HOME=/home/x/R2_Home", source)
        self.assertIn("Environment=R2_SERVICE_MODE=1", source)
        self.assertIn("Environment=R2_MODEL=gemma3:4b", source)
        self.assertIn("Environment=R2_OLLAMA_BASE_URL=http://127.0.0.1:11435", source)
        self.assertIn("BindsTo=r2-ollama.service", source)
        self.assertIn("Restart=always", source)
        self.assertIn("TimeoutStopSec=240", source)
        self.assertNotIn("R2_SVC_NETLOCK", source)

        model = (ROOT / "deploy/r2-ollama.service").read_text(encoding="utf-8")
        self.assertIn("User=r2", model)
        self.assertIn("OLLAMA_HOST=127.0.0.1:11435", model)
        self.assertIn("--uid-owner r2", model)
        self.assertIn("--dport 11435", model)
        self.assertIn("ip6tables", model)

    def test_interactive_launcher_uses_same_model_and_prevents_duplicate_core(self):
        launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
        self.assertIn('R2_MODEL="${R2_MODEL:-gemma3:4b}"', launcher)
        self.assertIn('R2_VISION_MODEL="$R2_MODEL"', launcher)
        self.assertIn('R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"', launcher)
        self.assertIn("systemctl show r2-ollama.service -p User", launcher)
        self.assertIn("systemctl is-active --quiet r2-3po.service", launcher)
        self.assertIn("pgrep -x r2", launcher)
        self.assertIn('TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"', launcher)
        self.assertIn('mv -f "$TEMP_EXEC" "$R2_EXEC"', launcher)
        self.assertIn('R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"', launcher)
        self.assertIn("Press Enter to continue launching R2, or Ctrl+C to cancel...", launcher)

    def test_installer_copies_model_locally_without_downloading(self):
        source = (ROOT / "tools/install_r2_overnight.sh").read_text(encoding="utf-8")
        self.assertIn("copy_ollama_model.py", source)
        self.assertIn("r2-ollama.service", source)
        self.assertIn("gemma3:4b", source)
        self.assertIn("integrity audit failed", source)
        self.assertIn("rollback_previous_executable", source)
        self.assertNotIn("ollama pull", source)
        self.assertNotIn("apt-get update", source)

if __name__ == "__main__":
    unittest.main()
