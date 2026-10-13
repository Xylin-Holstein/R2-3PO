#!/usr/bin/env python3
"""Static regression checks for the display-only V-Webcam integration."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> None:
    viewer = read("V-Webcam.py")
    eyes = read("Eyes.c")
    visual = read("Visual.c")
    launcher = read("R2_Launch_Code.sh")
    preflight = read("tools/preflight_r2_hardware.sh")
    workflow = read(".github/workflows/build.yml")

    assert "frame.ppm" in viewer and "focus.json" in viewer
    assert "Display only" in viewer and "Pause display" in viewer
    assert "not os.environ.get(\"DISPLAY\")" in viewer
    assert "DATA_DIR.lstat()" in viewer and "os.geteuid()" in viewer
    assert "if self.paused:" in viewer and "self.display_focus = self.focus()" in viewer
    assert 'if not self.display_status["active"] or not self.display_status["frames"]:' in viewer
    assert 'self.canvas.itemconfigure(self.image_item, image="")' in viewer
    assert "focus = self.display_focus" in viewer
    assert '#define R2_VWEBCAM_DIR "/tmp/r2-vwebcam-r2"' in eyes
    assert '#define R2_VWEBCAM_DIR "/tmp/r2-vwebcam-r2"' in visual
    assert "r2_vwebcam_publish_frame(eyes)" in eyes
    assert "r2_vwebcam_start_viewer()" in eyes and "r2_vwebcam_stop_viewer()" in eyes
    assert 'char *const argv[] = {"/usr/bin/python3", viewer, NULL};' in eyes
    assert "execv(argv[0], argv)" in eyes
    assert "PR_SET_PDEATHSIG" in eyes and "getppid() != parent_pid" in eyes
    assert "execlp(\"python3\"" not in eyes
    assert "R2_VWEBCAM_MAX_W 960U" in eyes
    assert "r2_vwebcam_last_publish_ms" in eyes
    assert "if (!display || !*display)" in eyes
    assert "Return ONLY valid JSON with keys description and focus" in visual
    assert '"format", json_object_new_string("json")' in visual
    assert "extract_visual_description(model_text)" in visual
    assert "publish_focus_region(focus)" in visual
    assert "const char *display = getenv(\"DISPLAY\")" in visual
    assert "if (!display || !*display)" in visual
    assert '"$R2_SOURCE/V-Webcam.py"' in launcher
    assert 'R2_SOURCE="$R2_SOURCE"' in launcher
    assert "V-Webcam.py" in preflight
    assert "python3 -m py_compile V-Webcam.py tests/vwebcam_smoke.py" in workflow
    print("V-Webcam mirror and focus overlay smoke checks passed.")


if __name__ == "__main__":
    main()
