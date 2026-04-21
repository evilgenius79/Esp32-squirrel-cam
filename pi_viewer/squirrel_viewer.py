#!/usr/bin/env python3
"""Squirrel-cam viewer for Raspberry Pi 5 + 7" DSI display.

Pulls the MJPEG video stream and the MLX90640 thermal JSON from the XIAO
ESP32S3, renders them side by side, and draws a min/max/centre overlay. Meant
to run fullscreen on the official 7" touchscreen (800x480) but adapts to any
resolution.

Usage:
    python3 squirrel_viewer.py --host squirrelcam.local
    python3 squirrel_viewer.py --host 192.168.1.42 --windowed
"""

from __future__ import annotations

import argparse
import json
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

import cv2
import numpy as np
import requests


# -----------------------------------------------------------------------------
# Shared state
# -----------------------------------------------------------------------------

@dataclass
class ThermalFrame:
    data: np.ndarray                 # shape (24, 32), Celsius
    tmin: float
    tmax: float
    seq: int
    stamp: float = field(default_factory=time.time)


class Shared:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.video_frame: Optional[np.ndarray] = None
        self.video_stamp: float = 0.0
        self.thermal: Optional[ThermalFrame] = None
        self.stop = threading.Event()


# -----------------------------------------------------------------------------
# Network workers
# -----------------------------------------------------------------------------

# Hard cap on the MJPEG reassembly buffer. A well-behaved VGA JPEG is under
# 100 KB; anything approaching this limit means the stream is corrupt or the
# server is malicious/broken, and we'd rather drop than OOM the Pi.
MJPEG_MAX_BUFFER = 4 * 1024 * 1024    # 4 MiB
MJPEG_MAX_FRAME  = 1 * 1024 * 1024    # 1 MiB


def mjpeg_reader(host: str, shared: Shared) -> None:
    """Continuously pull MJPEG frames. Parses the multipart stream by hand
    because requests' iter_content gives us raw bytes and OpenCV decodes
    JPEG faster than spinning up a heavier library."""
    url = f"http://{host}/stream"
    while not shared.stop.is_set():
        try:
            with requests.get(url, stream=True, timeout=5) as r:
                r.raise_for_status()
                buf = b""
                for chunk in r.iter_content(chunk_size=4096):
                    if shared.stop.is_set():
                        return
                    if not chunk:
                        continue
                    buf += chunk
                    # Discard the buffer if it blows past the cap without
                    # yielding a complete JPEG; a missing EOI marker would
                    # otherwise grow this unboundedly.
                    if len(buf) > MJPEG_MAX_BUFFER:
                        print(f"[mjpeg] buffer overflow ({len(buf)} B); resyncing")
                        buf = b""
                        continue
                    while True:
                        start = buf.find(b"\xff\xd8")
                        end = buf.find(b"\xff\xd9", start + 2) if start != -1 else -1
                        if start == -1 or end == -1:
                            break
                        frame_len = end + 2 - start
                        if frame_len > MJPEG_MAX_FRAME:
                            # Oversized frame — skip past the SOI and resync.
                            buf = buf[start + 2:]
                            continue
                        jpg = buf[start:end + 2]
                        buf = buf[end + 2:]
                        img = cv2.imdecode(
                            np.frombuffer(jpg, dtype=np.uint8),
                            cv2.IMREAD_COLOR,
                        )
                        if img is not None:
                            with shared.lock:
                                shared.video_frame = img
                                shared.video_stamp = time.time()
        except Exception as e:
            print(f"[mjpeg] {e}; retrying in 2s")
            time.sleep(2)


THERMAL_MAX_BYTES = 32 * 1024   # a well-formed payload is ~6 KB
THERMAL_W, THERMAL_H = 32, 24


def thermal_reader(host: str, shared: Shared, hz: float = 8.0) -> None:
    url = f"http://{host}/thermal"
    period = 1.0 / hz
    last_seq = -1
    while not shared.stop.is_set():
        t0 = time.time()
        try:
            # Stream the body so we can cap how many bytes we accept before
            # handing anything to the JSON parser.
            with requests.get(url, timeout=3, stream=True) as r:
                r.raise_for_status()
                body = bytearray()
                oversized = False
                for chunk in r.iter_content(chunk_size=4096):
                    if chunk:
                        body.extend(chunk)
                    if len(body) > THERMAL_MAX_BYTES:
                        oversized = True
                        break
            if oversized:
                print(f"[thermal] payload too large (>{THERMAL_MAX_BYTES} B); dropping")
                time.sleep(1)
                continue
            j = json.loads(body)

            if isinstance(j, dict) and "error" in j:
                print(f"[thermal] device says: {j['error']}")
                time.sleep(2)
                continue
            # Validate shape before trusting it.
            if (not isinstance(j, dict)
                    or j.get("w") != THERMAL_W or j.get("h") != THERMAL_H
                    or not isinstance(j.get("data"), list)
                    or len(j["data"]) != THERMAL_W * THERMAL_H):
                print("[thermal] unexpected payload shape; dropping")
                time.sleep(1)
                continue

            arr = np.asarray(j["data"], dtype=np.float32).reshape(THERMAL_H, THERMAL_W)
            seq = int(j.get("seq", last_seq + 1))
            if seq != last_seq:
                last_seq = seq
                frame = ThermalFrame(
                    data=arr, tmin=float(j["min"]), tmax=float(j["max"]), seq=seq
                )
                with shared.lock:
                    shared.thermal = frame
        except Exception as e:
            print(f"[thermal] {e}")
            time.sleep(1)
            continue

        dt = time.time() - t0
        if dt < period:
            time.sleep(period - dt)


# -----------------------------------------------------------------------------
# Rendering
# -----------------------------------------------------------------------------

def render_thermal(frame: ThermalFrame, size: tuple[int, int]) -> np.ndarray:
    """Return a BGR heatmap sized to (w, h) with a min/max bar drawn in."""
    w, h = size
    arr = frame.data
    span = max(frame.tmax - frame.tmin, 0.5)
    norm = np.clip((arr - frame.tmin) / span, 0.0, 1.0)
    norm8 = (norm * 255).astype(np.uint8)
    # MLX90640 is wired so (0,0) is top-right when looking at the sensor; flip
    # horizontally so the image matches the camera's orientation.
    norm8 = cv2.flip(norm8, 1)
    big = cv2.resize(norm8, (w, h), interpolation=cv2.INTER_CUBIC)
    color = cv2.applyColorMap(big, cv2.COLORMAP_INFERNO)

    # Mark hottest pixel (likely the squirrel).
    hot_idx = np.unravel_index(np.argmax(arr), arr.shape)
    hy, hx = hot_idx
    hx = (arr.shape[1] - 1) - hx  # account for the horizontal flip
    px = int((hx + 0.5) * w / arr.shape[1])
    py = int((hy + 0.5) * h / arr.shape[0])
    cv2.drawMarker(color, (px, py), (255, 255, 255), cv2.MARKER_CROSS, 18, 2)

    cv2.putText(color, f"{frame.tmin:5.1f}C",
                (6, h - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
    cv2.putText(color, f"{frame.tmax:5.1f}C",
                (w - 92, h - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
    return color


def compose(video: Optional[np.ndarray],
            thermal: Optional[ThermalFrame],
            out_w: int, out_h: int) -> np.ndarray:
    """Split-screen: video on the left, thermal on the right."""
    canvas = np.zeros((out_h, out_w, 3), dtype=np.uint8)
    split = out_w // 2

    if video is not None:
        vh, vw = video.shape[:2]
        scale = min(split / vw, out_h / vh)
        nw, nh = int(vw * scale), int(vh * scale)
        resized = cv2.resize(video, (nw, nh), interpolation=cv2.INTER_AREA)
        x = (split - nw) // 2
        y = (out_h - nh) // 2
        canvas[y:y + nh, x:x + nw] = resized
    else:
        cv2.putText(canvas, "waiting for video...",
                    (20, out_h // 2), cv2.FONT_HERSHEY_SIMPLEX, 0.8,
                    (80, 80, 80), 2)

    if thermal is not None:
        heat = render_thermal(thermal, (out_w - split, out_h))
        canvas[:, split:] = heat
    else:
        cv2.putText(canvas, "waiting for thermal...",
                    (split + 20, out_h // 2), cv2.FONT_HERSHEY_SIMPLEX, 0.8,
                    (80, 80, 80), 2)

    cv2.line(canvas, (split, 0), (split, out_h), (40, 40, 40), 1)
    return canvas


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="squirrelcam.local",
                    help="hostname or IP of the ESP32")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--windowed", action="store_true",
                    help="do not force fullscreen (useful for debugging)")
    args = ap.parse_args()

    shared = Shared()
    threads = [
        threading.Thread(target=mjpeg_reader,   args=(args.host, shared), daemon=True),
        threading.Thread(target=thermal_reader, args=(args.host, shared), daemon=True),
    ]
    for t in threads:
        t.start()

    win = "squirrel-cam"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    if not args.windowed:
        cv2.setWindowProperty(win, cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
    cv2.resizeWindow(win, args.width, args.height)

    last_fps_t = time.time()
    frames = 0
    fps = 0.0

    try:
        while True:
            with shared.lock:
                video = None if shared.video_frame is None else shared.video_frame.copy()
                thermal = shared.thermal

            view = compose(video, thermal, args.width, args.height)

            frames += 1
            now = time.time()
            if now - last_fps_t >= 1.0:
                fps = frames / (now - last_fps_t)
                frames = 0
                last_fps_t = now
            cv2.putText(view, f"{fps:4.1f} fps", (8, 18),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 255, 200), 1)

            cv2.imshow(win, view)
            key = cv2.waitKey(15) & 0xFF
            if key in (27, ord("q")):
                break
            if key == ord("f"):
                prop = cv2.getWindowProperty(win, cv2.WND_PROP_FULLSCREEN)
                cv2.setWindowProperty(
                    win, cv2.WND_PROP_FULLSCREEN,
                    cv2.WINDOW_NORMAL if prop == cv2.WINDOW_FULLSCREEN
                    else cv2.WINDOW_FULLSCREEN,
                )
    finally:
        shared.stop.set()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
