#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Print the rectangle of the first X11 window whose name contains a text, as WxH+X+Y, or fail.

Usage: x11-window-rect.py DISPLAY TEXT [SECONDS]

Waits up to SECONDS (10 by default) for the window to appear, so a script can start a game and then ask for its
window. The rectangle is in root coordinates, ready for ffmpeg's x11grab (DISPLAY+X,Y with -video_size WxH)."""

import sys
import time

from Xlib import display


def find(window, want, root):
    try:
        name = window.get_wm_name() or ""
        if want in name:
            geometry = window.get_geometry()
            # the root's origin in the window's space: negated, it is the window's origin on the root
            origin = window.translate_coords(root, 0, 0)
            return f"{geometry.width}x{geometry.height}+{-origin.x}+{-origin.y}"
        for child in window.query_tree().children:
            found = find(child, want, root)
            if found:
                return found
    except Exception:
        pass
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    connection = display.Display(sys.argv[1])
    root = connection.screen().root
    want = sys.argv[2]
    deadline = time.time() + (float(sys.argv[3]) if len(sys.argv) > 3 else 10.0)
    while time.time() < deadline:
        found = find(root, want, root)
        if found:
            print(found)
            return 0
        time.sleep(0.1)
    return 1


if __name__ == "__main__":
    sys.exit(main())
