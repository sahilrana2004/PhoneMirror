# Glass-to-glass latency test

Measures the full chain: iPhone render → encode → Wi-Fi → receive → decode → present → monitor scanout.

## Setup

1. iPhone: open a millisecond stopwatch web page (e.g. a stopwatch that shows ms, running full-screen) and set
   Auto-Lock to Never for the test.
2. PC: PhoneMirror mirroring the iPhone, window large enough that the digits are readable, **F3** stats on.
   Close other GPU/network-heavy apps. Note the codec, resolution and fps from the stats overlay.
3. Put the iPhone next to the monitor so both screens are in one camera frame.

## Measure

1. Film both screens with a second phone in **240 fps slow-motion** for ~10 seconds.
2. Step through the video frame by frame. Pick 10 frames spread across the clip where both stopwatches are
   readable (skip frames where a digit is mid-change).
3. For each, latency = iPhone reading − PC reading.
4. Report the **median** of the 10 samples (and min/max).

## Record

Add a row to `docs/latency-log.md` with: date, build (commit), connection (Wi-Fi band / router distance),
codec + resolution + fps, tearing on/off, windowed or fullscreen (F11), median / min / max, and the stats
overlay's "to screen" and "jitter" values.

Fullscreen (F11) lets Windows flip the swapchain directly ("independent flip"), skipping a composition
frame — measure both windowed and fullscreen.
