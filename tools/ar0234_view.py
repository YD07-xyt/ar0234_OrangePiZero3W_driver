#!/usr/bin/env python3
"""AR0234 RAW10 (V4L2 BA10, GRBG, 16-bit LE container) viewer / converter.

Runs on the host. Needs numpy + opencv-python.

Fast live preview (board shrinks to 8-bit RGB with ar0234_board_stream.py,
plain TCP, no SSH encryption on the video):
    # host
    nc -l 5000 | python3 ar0234_view.py live --fmt rgb8 --size 960x600 --wb
    # board
    v4l2-ctl -d /dev/video0 -i 0 --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
      --stream-mmap --stream-count=0 --stream-to=- 2>/dev/null \
      | python3 ~/ar0234_board_stream.py | nc 192.168.64.66 5000

Full-resolution RAW live preview over SSH (slow, ~2-4 fps on WiFi):
    ssh orangepi@192.168.64.10 'v4l2-ctl -d /dev/video0 -i 0 \
      --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
      --stream-mmap --stream-count=0 --stream-to=- 2>/dev/null' \
      | python3 ar0234_view.py live

Convert a captured file (one or more frames) to PNG:
    python3 ar0234_view.py png ar0234.raw --wb

Print statistics (mean / saturation) for exposure tuning:
    python3 ar0234_view.py stats ar0234.raw
"""
import argparse
import sys
import time

import numpy as np
import cv2

W, H = 1920, 1200
FRAME_BYTES = W * H * 2
# V4L2 GRBG (row0 = G R) == OpenCV "BayerGB" naming (checked with a synthetic pattern)
BAYER = cv2.COLOR_BayerGB2BGR
# Sensor data pedestal R0x301E = 42 (read back on the board). Without
# subtracting it, black shows as ~56/255 after gamma: the "haze".
BLACK = 42


def tone(bgr, wb, gamma, black, white):
    """bgr: float32 in sensor codes -> uint8, black-level, WB, gamma."""
    bgr = (bgr - black) / float(white - black)
    if wb:
        m = bgr.reshape(-1, 3).mean(0) + 1e-6
        bgr *= m[1] / m
    bgr = np.clip(bgr, 0, 1) ** (1.0 / gamma)
    return (bgr * 255).astype(np.uint8)


def to_bgr(raw, wb=False, gamma=2.2, black=BLACK):
    """raw: HxW uint16 (0..1023) -> BGR uint8."""
    return tone(cv2.cvtColor(raw, BAYER).astype(np.float32), wb, gamma, black, 1023)


def stats(raw):
    return "mean %.1f  median %.0f  p99 %.0f  sat %.2f%%" % (
        raw.mean(), np.median(raw), np.percentile(raw, 99),
        (raw >= 1023).mean() * 100)


def skip_header(f):
    # `v4l2-ctl -i 0 --stream-to=-` prints "Video input set to 0 (...)\n"
    # to stdout before the frame data.
    head = f.peek(64)[:64] if hasattr(f, "peek") else b""
    if head.startswith(b"Video input"):
        f.readline()


def read_exact(f, n):
    d = f.read(n)
    while d is not None and 0 < len(d) < n:
        more = f.read(n - len(d))
        if not more:
            break
        d += more
    return d if d and len(d) == n else None


def frames_from(f, skip_text=False):
    if skip_text:
        skip_header(f)
    while True:
        d = read_exact(f, FRAME_BYTES)
        if d is None:
            return
        yield np.frombuffer(d, "<u2").reshape(H, W)


def cmd_live(a):
    f = sys.stdin.buffer
    skip_header(f)
    if a.fmt == "rgb8":
        w, h = map(int, a.size.split("x"))
        n = w * h * 3
    t0, cnt, fps = time.time(), 0, 0.0
    i = 0
    while True:
        if a.fmt == "raw10":
            d = read_exact(f, FRAME_BYTES)
            if d is None:
                break
            raw = np.frombuffer(d, "<u2").reshape(H, W)
            img = to_bgr(raw, a.wb, black=a.black)
            if a.scale != 1:
                img = cv2.resize(img, None, fx=a.scale, fy=a.scale,
                                 interpolation=cv2.INTER_AREA)
            info = stats(raw)
        else:
            d = read_exact(f, n)
            if d is None:
                break
            rgb = np.frombuffer(d, np.uint8).reshape(h, w, 3)
            # board already subtracted black and scaled to 0..255 linear
            img = tone(rgb[..., ::-1].astype(np.float32), a.wb, 2.2, 0, 255)
            info = "mean %.1f (8-bit linear)  sat %.2f%%" % (
                rgb.mean(), (rgb >= 255).mean() * 100)
        cnt += 1
        if time.time() - t0 >= 1:
            fps, cnt, t0 = cnt / (time.time() - t0), 0, time.time()
        if a.stats:
            cv2.putText(img, "%.1f fps  %s" % (fps, info), (10, 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 1)
        cv2.imshow("ar0234", img)
        k = cv2.waitKey(1) & 0xFF
        if k in (27, ord("q")):
            break
        if k == ord("s"):
            cv2.imwrite("live_%04d.png" % i, img)
            if a.fmt == "raw10":
                raw.tofile("live_%04d.raw" % i)
            print("saved live_%04d" % i)
        i += 1


def cmd_png(a):
    base = a.file.rsplit(".", 1)[0]
    with open(a.file, "rb") as f:
        for i, raw in enumerate(frames_from(f, skip_text=True)):
            out = "%s_%03d.png" % (base, i)
            cv2.imwrite(out, to_bgr(raw, a.wb, black=a.black))
            print(out, stats(raw))
            if i + 1 >= a.max:
                break


def cmd_stats(a):
    f = sys.stdin.buffer if a.file == "-" else open(a.file, "rb")
    with f:
        for i, raw in enumerate(frames_from(f, skip_text=True)):
            print(i, stats(raw))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("live", help="preview frames from stdin (q/Esc quit, s save)")
    s.add_argument("--fmt", choices=("raw10", "rgb8"), default="raw10",
                   help="raw10: v4l2-ctl output; rgb8: ar0234_board_stream.py output")
    s.add_argument("--size", default="960x600",
                   help="rgb8 frame size (960x600 for --bin 1, 480x300 for --bin 2)")
    s.add_argument("--scale", type=float, default=0.5, help="raw10 display scale")
    s.add_argument("--wb", action="store_true", help="gray-world white balance")
    s.add_argument("--stats", action="store_true", help="overlay fps/mean/saturation")
    s.add_argument("--black", type=int, default=BLACK, help="raw10 black level")
    s.set_defaults(fn=cmd_live)
    s = sub.add_parser("png", help="convert RAW file to PNG")
    s.add_argument("file")
    s.add_argument("--wb", action="store_true")
    s.add_argument("--max", type=int, default=10)
    s.add_argument("--black", type=int, default=BLACK)
    s.set_defaults(fn=cmd_png)
    s = sub.add_parser("stats", help="per-frame statistics")
    s.add_argument("file")
    s.set_defaults(fn=cmd_stats)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
