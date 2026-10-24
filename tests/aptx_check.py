#!/usr/bin/env python3
"""aptX encoder check: ours vs FFmpeg's encoder (byte for byte), then the
SNR of our stream decoded by FFmpeg against the reference PCM.
Usage: aptx_check.py ours.aptx ffmpeg.aptx ref.s16 decoded.s16"""
import sys
import numpy as np

ours = open(sys.argv[1], 'rb').read()
ff = open(sys.argv[2], 'rb').read()
n = min(len(ours), len(ff))
diff = next((i for i in range(n) if ours[i] != ff[i]), None)
bad = 0
if diff is not None or len(ours) != len(ff):
    print(f"FAIL aptX stream differs from FFmpeg at byte {diff} (ours {len(ours)} B, FFmpeg {len(ff)} B)")
    bad = 1
else:
    print(f"ok   aptX stream identical to FFmpeg's encoder ({len(ours)} bytes)")

ref = np.fromfile(sys.argv[3], '<i2').astype(float).reshape(-1, 2)
dec = np.fromfile(sys.argv[4], '<i2').astype(float).reshape(-1, 2)
best = None
for d in range(0, 200):
    m = min(len(ref), len(dec) - d) - 2000
    r = ref[1000:m]
    x = dec[1000 + d:m + d]
    err = np.sum((r - x) ** 2)
    if best is None or err < best[1]:
        best = (d, err, r, x)
d, err, r, x = best
for ch in range(2):
    snr = 10 * np.log10(np.sum(r[:, ch] ** 2) / np.sum((r[:, ch] - x[:, ch]) ** 2))
    ok = snr >= 30
    bad |= not ok
    print(f"{'ok  ' if ok else 'FAIL'} aptX decoded by FFmpeg ch{ch}: delay {d} samples, SNR {snr:.1f} dB (need 30)")
sys.exit(bad)
