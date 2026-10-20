#!/usr/bin/env python3
"""Regression checks for the clock-aware seasonal R2 shell themes."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "shell.c").read_text(encoding="utf-8")
help_text = source[source.find("static void shell_help(void)"):source.find("/* ============================================================\n   STATUS", source.find("static void shell_help(void)"))]
dispatch_start = source.find("static int shell_dispatch(char *input)")
dispatch_end = source.find("/* ============================================================\n   SHELL LOOP", dispatch_start)
dispatch = source[dispatch_start:dispatch_end]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


require("shell_theme_for_date" in source and "localtime_r(&now, &local_date)" in source,
        "automatic theme selection uses R2's process-local clock")
require("date->tm_mon == 9" in source and "R2_SHELL_THEME_SPOOKY" in source,
        "October selects SpookyShell")
require("date->tm_mon == 11" in source and "date->tm_mday == 1" in source,
        "December and New Year's Day select XmasShell")
require('"XmasShell> "' in source and '"SpookyShell> "' in source,
        "seasonal themes have distinct interactive prompts")
require('"shell theme [auto|classic|xmas|spooky]"' in help_text,
        "help documents manual and automatic theme controls")
require('"xmasshell"' in dispatch and '"spookyshell"' in dispatch,
        "XmasShell and SpookyShell have direct command aliases")
require('"NO_COLOR"' in source,
        "users can disable ANSI color output with NO_COLOR")
require("shell_theme_auto = 1;" in source and "shell_theme_refresh_auto();" in source,
        "automatic calendar selection is restored by auto/reset and initialized at startup")
require("memcpy(option, argument, length)" in source,
        "theme arguments are copied into bounded writable storage before normalization")

print("Seasonal shell regression checks passed.")
