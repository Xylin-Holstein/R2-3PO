# R2-3PO V-Webcam

V-Webcam is a **display-only monitor** for R2's existing Eyes frames. It does not open a second camera/FFmpeg stream and does not participate in perception, reasoning, or memory.

## How the connection works

1. `Eyes.c` publishes the RGB24 frame it has just captured as an atomic PPM update in `/tmp/r2-vwebcam-r2/frame.ppm`, throttled to at most about two display updates per second.
2. `Eyes.c` writes `status.txt` with the source, native dimensions, and captured-frame count. The status is marked inactive when Eyes closes.
3. `V-Webcam.py` polls those files. Its window stays hidden until Eyes reports an active VLC or physical webcam source, then appears with the latest frame. It hides again when that source closes.
4. `R2_Launch_Code.sh` starts the viewer as the dedicated `r2` user and passes a private copy of the desktop X11 authorization cookie. It does not use `xhost +`.

The directory is mode 0700 and owned by `r2`; frames are not exposed to other local accounts.

## Requirements

On Ubuntu with a graphical X11 session:

```bash
sudo apt install python3-tk xauth
python3 -c "import tkinter; print('Tkinter OK')"
```

Run R2 through `R2_Launch_Code.sh` from the logged-in desktop session. V-Webcam opens in a waiting state at startup and changes to **EYES ACTIVE** when Eyes opens a source. If its window fails, inspect `/home/x/R2_Home/V-Webcam.log`.

## Reading the indicator

- **WAITING FOR VLC / WEBCAM**: no eligible VLC or webcam source is active. Opening an ordinary image/video file does not trigger the popup.
- **EYES ACTIVE**: Eyes reports an open source; the frame counter and timestamped frame display help verify that actual pixels are arriving.
- The display is a monitor, not proof that a vision model successfully interpreted a frame. R2's sensory logs and visual-experience records remain the evidence for analysis.
