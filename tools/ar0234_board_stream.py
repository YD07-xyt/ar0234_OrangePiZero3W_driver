#!/usr/bin/env python3
"""Board side: read AR0234 RAW10 frames from stdin, shrink, write 8-bit to stdout.

Runs on the Orange Pi (needs python3-numpy). Pair with
`ar0234_view.py live --fmt rgb8` on the host.

Each output frame is (H/2/bin) x (W/2/bin) x 3 bytes, RGB, 8-bit:
every 2x2 GRBG cell becomes one RGB pixel (half resolution, nearest sample,
no interpolation); `--bin N` decimates further by N.

    v4l2-ctl -d /dev/video0 -i 0 --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
      --stream-mmap --stream-count=0 --stream-to=- 2>/dev/null \
      | python3 ar0234_board_stream.py --bin 1 --every 2
"""
import argparse
import sys

import numpy as np

W, H = 1920, 1200
FRAME_BYTES = W * H * 2


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin", type=int, default=1,
                   help="extra NxN decimation after 2x2 -> RGB (1: 960x600, 2: 480x300)")
    p.add_argument("--every", type=int, default=1, help="send 1 of every N frames")
    p.add_argument("--black", type=int, default=42,
                   help="black level subtracted (R0x301E pedestal = 42)")
    a = p.parse_args()

    inp, out = sys.stdin.buffer, sys.stdout.buffer
    head = inp.peek(64)[:64]
    if head.startswith(b"Video input"):
        inp.readline()

    buf = bytearray(FRAME_BYTES)
    mv = memoryview(buf)
    # 10-bit code -> 8-bit linear, black subtracted
    lut = np.clip((np.arange(1024) - a.black) * 255.0 / (1023 - a.black),
                  0, 255).astype(np.uint8)
    oh, ow = H // 2 // a.bin, W // 2 // a.bin
    out_img = np.empty((oh, ow, 3), np.uint8)
    n = 0
    while True:
        got = 0
        while got < FRAME_BYTES:
            r = inp.readinto(mv[got:])
            if not r:
                return
            got += r
        n += 1
        if (n - 1) % a.every:
            continue
        raw = np.frombuffer(buf, "<u2").reshape(H, W)
        # GRBG: row0 = G R, row1 = B G. Take one sample per channel per
        # 2*bin cell (decimation; G uses the row-0 green only).
        s = 2 * a.bin
        out_img[..., 0] = lut[raw[0:H:s, 1:W:s]]
        out_img[..., 1] = lut[raw[0:H:s, 0:W:s]]
        out_img[..., 2] = lut[raw[1:H:s, 0:W:s]]
        try:
            out.write(out_img.data)
            out.flush()
        except BrokenPipeError:
            return


if __name__ == "__main__":
    main()
