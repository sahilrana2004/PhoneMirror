# Latency log

Glass-to-glass numbers come from the procedure in `latency-test.md` (median of 10, ms). PC-side numbers come
from the pipeline's own timers (receive callback → `Present()` returned; excludes DWM and scanout).

## PC-side pipeline (local, `--play` test streams, RTX 5060 Ti, 60 fps)

| Date | Build | Stream | Decode submit | Receive → present (avg / max) | Dropped |
|---|---|---|---|---|---|
| 2026-10-25 | d61af99 (M1) | H.265 994×2160 | 0.25 ms | 0.34 / 0.88 ms | 0 |
| 2026-10-25 | d61af99 (M1) | H.264 496×1080 | 0.23 ms | 0.32 / 0.52 ms | 0 |
| 2026-10-25 | a6d7bc5 (M2) | H.265 994×2160 | 0.28 ms | 0.78 / 3.4 ms (F3 overlay + recording active) | 1 |

## Glass-to-glass (iPhone 17 Pro → PC over Wi-Fi)

| Date | Build | Network | Codec / size / fps | Mode | Median | Min / max | Notes |
|---|---|---|---|---|---|---|---|
| | | | | | | | pending device measurement |
