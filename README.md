# PhoneMirror

Low-latency iPhone screen mirroring and control for Windows. The iPhone mirrors to the PC over AirPlay
(Control Center › Screen Mirroring › PhoneMirror); the PC's mouse and keyboard control the iPhone over the PC's own
Bluetooth. Native C++20 / Win32 / Direct3D 11, single portable exe.

- Zero-copy hardware decode (D3D11VA, H.264 and H.265 up to 2160 lines), no jitter buffer, latency-1 flip-model
  swapchain with tearing, newest-frame-wins presentation, MMCSS threads, Wi-Fi streaming mode while mirroring.
- Window styled after macOS iPhone Mirroring: phone-shaped, aspect-locked, follows rotation, hover title bar
  (Home, App Switcher, screenshot, record, pin, mute, stats, settings, close).
- Audio (AAC-ELD/ALAC/AAC) via WASAPI with a 20 ms buffer; MP4 recording without re-encoding the video;
  PNG screenshots; several phones at once (one window each).
- Mouse/keyboard control as a Bluetooth LE HID device — no dongle.

Docs: [control setup](docs/control-setup.md) · [troubleshooting](docs/troubleshooting.md) ·
[latency test](docs/latency-test.md) · [latency log](docs/latency-log.md)

## Build

Requirements: Visual Studio 2022 or 2026 with the C++ desktop workload (includes CMake and vcpkg), Windows SDK
10.0.22621+. From a *Developer PowerShell for VS*:

```powershell
cmake --preset vs
cmake --build --preset release
```

Dependencies (OpenSSL, libplist, a trimmed FFmpeg via `ports/ffmpeg`) are built statically by vcpkg on first
configure (`x64-windows-static`). Output: `build\Release\PhoneMirror.exe` — no DLLs or runtime needed.

Installer (Inno Setup 6): `ISCC.exe installer\phonemirror.iss` → `build\installer\PhoneMirror-Setup-<ver>.exe`
(adds the firewall rule, optional start-with-Windows).

## Settings

`%APPDATA%\PhoneMirror\settings.json` (tray › Open settings file; restart to apply):

| Key | Default | |
|---|---|---|
| `receiver_name` | `"PhoneMirror"` | Name shown in Screen Mirroring |
| `receivers` | `1` | Phones at once (1–4); extra ones appear as "PhoneMirror (2)" … |
| `codec` | `"auto"` | `"h264"` forces H.264 (1080 lines max) |
| `max_fps`, `refresh_rate` | `60`, `60` | Advertised to the iPhone |
| `tearing` | `true` | Lowest latency; `false` for tear-free |
| `always_on_top`, `stats` | `false` | Also toggled from the hover bar |
| `swap_ctrl_cmd` | `true` | Ctrl acts as Command on the iPhone |
| `pointer_gain` | `1.0` | Mouse speed on the iPhone |
| `screenshot_to_clipboard` | `true` | |
| `audio_follows_focus` | `true` | With several phones, only the front window plays |
| `audio_delay_ms` | `0` | "Sync audio to video": hold audio back (0–500 ms) to match the picture |
| `pointer_cal_x`, `pointer_cal_y` | — | Written by pointer calibration (Ctrl+Shift+P) |

## Developer flags

`--play <file.h264|.h265>` and `--play-audio <file.m4a>` replay files through the live pipelines;
`--version` prints the version.

## License

GPL-3.0 (see `LICENSE`). Includes the AirPlay protocol layer of [UxPlay](https://github.com/FDH2/UxPlay)
(GPL-3.0, `src/airplay/uxplay`, see `UPSTREAM`), [mdns](https://github.com/mjansson/mdns) (public domain) and links
FFmpeg (LGPL), OpenSSL (Apache-2.0) and libplist (LGPL-2.1).
