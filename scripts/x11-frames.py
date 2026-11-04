#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Capture a crop of an X11 window's frames as PPMs, as fast as the X server gives them.

Usage: x11-frames.py DISPLAY TEXT X Y W H SECONDS OUT_DIR [PREFIX]

Finds the first window whose name contains TEXT, waits up to 20 s for it, then grabs the WxH crop at X,Y (window
coordinates) until SECONDS pass, writing PREFIX00000.ppm and so on. The frames are what the window showed, which for a
Vulkan window on Xwayland is the presented image (x11grab cannot see it, but XGetImage can)."""

import os
import sys
import time

from Xlib import X, display
from PIL import Image


def find(window, want):
    try:
        name = window.get_wm_name() or ""
        if want in name:
            return window
        for child in window.query_tree().children:
            found = find(child, want)
            if found:
                return found
    except Exception:
        pass
    return None


def main():
    if len(sys.argv) < 9:
        print(__doc__, file=sys.stderr)
        return 2
    connection = display.Display(sys.argv[1])
    want = sys.argv[2]
    x, y, w, h = (int(v) for v in sys.argv[3:7])
    seconds = float(sys.argv[7])
    out = sys.argv[8]
    prefix = sys.argv[9] if len(sys.argv) > 9 else "f"
    os.makedirs(out, exist_ok=True)
    window = None
    deadline = time.time() + 20
    while window is None and time.time() < deadline:
        window = find(connection.screen().root, want)
        if window is None:
            time.sleep(0.1)
    if window is None:
        print("x11-frames: window not found", file=sys.stderr)
        return 1
    stop = time.time() + seconds
    n = 0
    while time.time() < stop:
        raw = window.get_image(x, y, w, h, X.ZPixmap, 0xFFFFFFFF)
        image = Image.frombytes("RGB", (w, h), raw.data, "raw", "BGRX")
        image.save(os.path.join(out, f"{prefix}{n:05d}.ppm"))
        n += 1
    print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
