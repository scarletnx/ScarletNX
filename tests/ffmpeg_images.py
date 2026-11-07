# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""FFmpeg's image codecs, libswscale and zlib are the Android driver's (cmake/ffmpeg.cmake): built with it, and only
then. PRIVATE is the folder of the configuration FFmpeg was built with (config.h and config_components.h), WANT 1 in a
build with the driver and 0 without."""

import re
import sys
from pathlib import Path

NAMES = (
    'CONFIG_PNG_DECODER',
    'CONFIG_MJPEG_DECODER',
    'CONFIG_WEBP_DECODER',
    'CONFIG_GIF_DECODER',
    'CONFIG_BMP_DECODER',
    'CONFIG_PNG_ENCODER',
    'CONFIG_MJPEG_ENCODER',
    'CONFIG_SWSCALE',
    'CONFIG_ZLIB',
)


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[2] not in ('0', '1'):
        print('usage: ffmpeg_images.py PRIVATE 0|1', file=sys.stderr)
        return 2
    path, want = Path(sys.argv[1]), sys.argv[2]
    text = (path / 'config.h').read_text() + (path / 'config_components.h').read_text()
    values = dict(re.findall(r'^#define (CONFIG_\w+) (\d+)$', text, re.MULTILINE))
    wrong = [f'{name} is {values.get(name, "not defined")}, not {want}' for name in NAMES if values.get(name) != want]
    for line in wrong:
        print(f'ffmpeg_images: {path}: {line}', file=sys.stderr)
    if not wrong:
        print(
            f"ffmpeg_images: {len(NAMES)} of FFmpeg's settings are {want}, as the Android driver is {'on' if want == '1' else 'off'}"
        )
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
