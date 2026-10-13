#!/usr/bin/env python3
"""Static checks for the dedicated r2-owned model service and isolated R2 runtime."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OvernightServiceTests(unittest.TestCase):
    def test_core_and_shell_lifecycle(self):
        core = (ROOT / "r2.c").read_text(encoding="utf-8")
        shell = (ROOT / "shell.c").read_text(encoding="utf-8")
        self.assertIn("reality_needs_worker", core)
        self.assertIn("r2_reality_autonomous_feed_if_needed()", core)
        self.assertIn("R2_OLLAMA_BASE_URL", core)
        self.assertIn("append_runtime_context(&memory_context)", core)
        self.assertIn("tv_control_start()", shell)
        self.assertIn("tv_control_stop_server()", shell)
        self.assertIn("static atomic_int tv_control_stop", shell)
        self.assertIn("accept(server_fd", shell)
        self.assertIn("MSG_NOSIGNAL", shell)
        self.assertIn("R2_SERVICE_MODE", shell)

    def test_service_runner_uses_gemma_and_private_local_endpoint(self):
        source = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
        self.assertIn('R2_MODEL="${R2_MODEL:-gemma3:4b}"', source)
        self.assertIn('R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"', source)
        self.assertIn('OLLAMA_TAGS_URL="$R2_OLLAMA_BASE_URL/api/tags"', source)
        self.assertIn("preflight_ollama_model.py", source)
        self.assertIn("--wait-seconds 60 --timeout 180", source)
        self.assertIn("LD_PRELOAD", source)
        self.assertIn("FAKETIME_DONT_RESET=1", source)
        self.assertIn("R2_SERVICE_MODE=1", source)
        self.assertIn("Imagination.c", (ROOT / "Makefile").read_text(encoding="utf-8"))
        self.assertIn("systemd-inhibit --what=sleep", source)
        self.assertNotIn("qwen3.5:9b", source)
        self.assertNotIn("apt-get update", source)
        self.assertNotIn("ollama pull", source)

    def test_ollama_server_is_a_separate_r2_owned_isolated_service(self):
        server = (ROOT / "deploy/r2-ollama.service").read_text(encoding="utf-8")
        app = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
        self.assertIn("User=r2", server)
        self.assertIn("OLLAMA_HOST=127.0.0.1:11435", server)
        self.assertIn("OLLAMA_MODELS=/home/x/R2_Home/.ollama/models", server)
        self.assertIn("--uid-owner r2", server)
        self.assertIn("--dport 11435", server)
        self.assertIn("ip6tables", server)
        self.assertIn("ExecStopPost=", server)
        self.assertIn("User=r2", app)
        self.assertIn("BindsTo=r2-ollama.service", app)
        self.assertIn("R2_OLLAMA_BASE_URL=http://127.0.0.1:11435", app)
        self.assertNotIn("ExecStartPre=+", app)
        self.assertNotIn("R2_SVC_NETLOCK", app)
        self.assertIn("Restart=always", app)

    def test_interactive_launcher_and_installer_enforce_model_ownership(self):
        launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
        installer = (ROOT / "tools/install_r2_overnight.sh").read_text(encoding="utf-8")
        self.assertIn('R2_MODEL="${R2_MODEL:-gemma3:4b}"', launcher)
        self.assertIn("systemctl start r2-ollama.service", launcher)
        self.assertIn('R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"', launcher)
        self.assertIn("sudo -n -u r2 -- gcc -std=c11 -Wall -Wextra -Werror -O2", launcher)
        self.assertIn("preflight_ollama_model.py", launcher)
        self.assertIn('TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"', launcher)
        self.assertIn("copy_ollama_model.py", installer)
        self.assertIn("r2-ollama.service", installer)
        self.assertIn("systemctl enable --now r2-ollama.service", installer)
        self.assertIn("run_as_r2 make -C", installer)
        self.assertNotIn("apt-get update", installer)
        self.assertNotIn("ollama pull", installer)


if __name__ == "__main__":
    unittest.main()
