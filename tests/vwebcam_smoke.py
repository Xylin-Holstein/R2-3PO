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
    assert "return total == 0 ? 0 : -1;" in eyes, (
        "partial raw frames must be reported as read failures, not clean EOF"
    )
    capture_start = eyes.index("int r2_eyes_capture(")
    capture_end = eyes.index("/* ============================================================\n   FRAME ACCESS", capture_start)
    capture = eyes[capture_start:capture_end]
    eof_branch = capture[capture.index("if (result == 0)"):capture.index("if (result < 0)")]
    assert "r2_eyes_close(eyes);" in eof_branch, (
        "EOF from a live camera or file must close the dead FFmpeg stream"
    )
    assert "if (!eyes->source_is_live)" not in eof_branch, (
        "a dead live-camera pipe must not remain marked open"
    )
    assert 'char *const argv[] = {"/usr/bin/python3", viewer, NULL};' in eyes
    assert "execv(argv[0], argv)" in eyes
    assert "PR_SET_PDEATHSIG" in eyes and "getppid() != parent_pid" in eyes
    assert "execlp(\"python3\"" not in eyes
    assert 'getenv("R2_CAMERA_DEVICE")' in eyes
    assert 'R2_CAMERA_DEVICE="${R2_CAMERA_DEVICE:-/dev/video0}"' in launcher
    assert "R2_CAMERA_DEVICE" in read("docs/local-speech-input.md")
    assert "R2_VWEBCAM_MAX_W 960U" in eyes
    assert "r2_vwebcam_last_publish_ms" in eyes
    assert "if (!display || !*display)" in eyes
    assert "Return ONLY valid JSON with keys description and focus" in visual
    assert '"format", json_object_new_string("json")' in visual
    assert "extract_visual_description(model_text)" in visual
    assert "static pthread_mutex_t focus_publish_lock = PTHREAD_MUTEX_INITIALIZER;" in visual
    focus_writer = visual[visual.index("static void publish_focus_region(json_object *focus)"):visual.index("/* Return the natural-language description", visual.index("static void publish_focus_region(json_object *focus)"))]
    assert "pthread_mutex_lock(&focus_publish_lock);" in focus_writer
    assert "publish_focus_region_impl(focus);" in focus_writer
    assert "pthread_mutex_unlock(&focus_publish_lock);" in focus_writer, (
        "concurrent vision requests must serialize writes to the shared focus temporary file"
    )
    assert "static int visual_insert_compat(" in visual
    assert "visual_insert_compat(created, source, image_path, model_snapshot," in visual
    for legacy_column in ("created_at", "timestamp", "file_path", "path"):
        assert f'visual_column_exists("{legacy_column}")' in visual
    assert 'visual_backfill("image_path", "file_path")' in visual
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
