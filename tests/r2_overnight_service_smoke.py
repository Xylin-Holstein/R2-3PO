#!/usr/bin/env python3
"""Static safety/regression checks for R2's unattended overnight service."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OvernightServiceTests(unittest.TestCase):
    def test_shell_has_signal_driven_noninteractive_mode(self):
        source = (ROOT / "shell.c").read_text(encoding="utf-8")
        self.assertIn('getenv("R2_SERVICE_MODE")', source)
        self.assertIn('!strcmp(service_mode_value, "1")', source)
        self.assertIn("if (service_mode) {", source)
        self.assertIn("pause();", source)
        self.assertIn("SIGTERM", source)
        core = (ROOT / "r2.c").read_text(encoding="utf-8")
        self.assertIn("r2_shutdown();", core)
        self.assertIn("setvbuf(stdout, NULL, _IOLBF, 0)", core)

    def test_runner_checks_local_model_and_private_clock(self):
        source = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
        self.assertIn("gemma3:4b", source)
        self.assertIn("refusing symlinked R2 database", source)
        self.assertIn("r2_fridge.db", source)
        self.assertIn("/api/tags", source)
        self.assertIn("LD_PRELOAD", source)
        self.assertIn("FAKETIME_DONT_RESET=1", source)
        self.assertIn("R2_SERVICE_MODE=1", source)
        self.assertIn("systemd-inhibit --what=sleep", source)
        self.assertLess(source.index("exec systemd-inhibit"), source.index('log "Waiting for the local Ollama API..."'))
        self.assertNotIn("apt-get update", source)
        self.assertNotIn("ollama pull", source)

    def test_unit_restarts_failures_and_runs_as_dedicated_user(self):
        source = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
        self.assertIn("User=r2", source)
        self.assertIn("Restart=always", source)
        self.assertIn("RestartSec=10", source)
        self.assertIn("TimeoutStopSec=240", source)
        self.assertIn("WantedBy=multi-user.target", source)
        self.assertIn("R2_SERVICE_MODE=1", source)
        launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
        self.assertIn("systemctl is-active --quiet r2-3po.service", launcher)
        self.assertIn("two R2 cores", launcher)

    def test_installer_is_non_destructive_and_does_not_update_apt(self):
        source = (ROOT / "tools/install_r2_overnight.sh").read_text(encoding="utf-8")
        self.assertIn("r2.new", source)
        self.assertIn('mv -f "$TEMP_EXEC" "$R2_EXEC"', source)
        self.assertIn("systemctl enable --now r2-3po.service", source)
        self.assertNotIn("apt-get update", source)
        self.assertIn("pgrep -x r2", source)
        self.assertIn("FAKETIME_FOUND", source)
        self.assertIn("refusing symlinked R2 database", source)
        self.assertIn("audit-snapshot-", source)
        self.assertIn("integrity audit failed", source)


if __name__ == "__main__":
    unittest.main()
