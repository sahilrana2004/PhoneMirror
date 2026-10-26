# Troubleshooting

Logs: `%APPDATA%\PhoneMirror\phonemirror.log` (rewritten each start). Crash dumps: `%APPDATA%\PhoneMirror\crashes`.
Settings: `%APPDATA%\PhoneMirror\settings.json` (tray › Open settings file; restart to apply).

## "PhoneMirror" does not appear in Screen Mirroring

- PC and iPhone must be on the same Wi-Fi network (not a guest network with client isolation).
- Network profile should be **Private**: Settings › Network & internet › Wi-Fi › your network › Network profile type.
- Firewall: tray › *Check network & firewall setup* adds an inbound rule (asks for admin). The installer adds it too.
- Another AirPlay receiver (LetsView, AirServer, UxPlay) using TCP 7000 blocks PhoneMirror's slot 0 — quit it,
  or the log says "cannot listen on TCP 7000".
- Bonjour may be installed or not; PhoneMirror has its own mDNS responder and works either way.

## Lowest latency on Wi-Fi

PhoneMirror already switches the PC's Wi-Fi adapter to *media streaming mode* and disables background scans while
mirroring (both revert automatically). Further, in Device Manager › network adapter › Advanced:

- **Roaming Aggressiveness**: Lowest
- **MIMO / Power Saving Mode**: Maximum Performance (or "No SMPS")
- **Preferred Band**: 5 GHz or 6 GHz
- **Throughput Booster**: Enabled (if present)

**Wired option (cheapest big win):** an iPhone with USB-C accepts a USB-C Ethernet adapter. Plug the iPhone into the
router with it, put the PC on Ethernet too, and AirPlay runs over the wire — no radio jitter, same PhoneMirror
setup (both must be on the same network/subnet).

Also: Power Options › Wireless Adapter Settings › Power Saving Mode: *Maximum Performance*. Keep the router close,
use 5/6 GHz on an uncongested channel, and avoid mesh hops between phone and PC. The F3 overlay's *jitter* value
shows how evenly frames arrive; spikes point at Wi-Fi.

## Video

- **F3** shows codec, size and fps. H.265 at up to 2160 lines is used when the GPU decodes HEVC; set
  `"codec": "h264"` to force H.264 (1080 lines max, a little less work on both ends).
- `"tearing": false` if fast motion shows tear lines (adds up to one refresh of latency).
- Fullscreen (F11) is the lowest-latency display path.

## Audio

- Audio plays through the Windows default output and follows device changes. The iPhone volume buttons control it;
  the speaker button in the hover bar mutes, mouse wheel over it changes PhoneMirror's own volume.
- With several phones, only the window in front plays sound; set `"audio_follows_focus": false` to hear all.
- Audio is played the moment it arrives and is never held back for the picture. If lips look out of sync (sound
  early), set `"audio_delay_ms"` (e.g. 40–80) to delay the sound.

## Control (mouse/keyboard)

See `control-setup.md`.
