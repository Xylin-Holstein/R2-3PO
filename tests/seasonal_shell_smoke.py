#!/usr/bin/env python3
"""Regression contract for automatic and manually selectable seasonal shells."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "shell.c").read_text(encoding="utf-8")
theme_start = source.index("static const char *shell_theme(void)")
theme_end = source.index("static void shell_banner(void)", theme_start)
theme = source[theme_start:theme_end]
banner_start = theme_end
banner_end = source.index("/* ============================================================\n   HELP", banner_start)
rendering = source[banner_start:banner_end]

assert 'getenv("R2_SHELL_THEME")' in theme
assert '"xmas"' in theme and '"christmas"' in theme and '"xmasshell"' in theme
assert '"spooky"' in theme and '"halloween"' in theme and '"spookyshell"' in theme
assert 'local_now.tm_mon == 9' in theme
assert 'local_now.tm_mon == 11' in theme
assert 'localtime_r(&now, &local_now)' in theme
assert 'return "classic";' in theme
assert "SPOOKYSHELL" in rendering and "XMAS SHELL" in rendering
assert '"[SPOOKY] R2> "' in rendering and '"[XMAS] R2> "' in rendering
assert "R2_SHELL_THEME" in theme
print("seasonal shell smoke passed")
