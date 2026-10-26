# Controlling the iPhone with the PC's mouse and keyboard

PhoneMirror makes the PC's own Bluetooth radio look like a Bluetooth keyboard + mouse to the iPhone (BLE HID).
No dongle is needed. Windows allows third-party apps to publish the HID service; only the Device Information
service is reserved, which iOS does not require.

## One-time setup

1. Start PhoneMirror (it advertises the keyboard/mouse while running).
2. iPhone: **Settings › Bluetooth** — connect to this PC. If the PC is already listed under My Devices (e.g.
   from Phone Link), tap it; if the keyboard/mouse never connects, *Forget This Device* on the iPhone, remove the
   iPhone in Windows **Settings › Bluetooth & devices**, and pair again from the iPhone's Other Devices list.
3. iPhone: **Settings › Accessibility › Touch › AssistiveTouch** — on. This is what shows a pointer.
   Optional: *Pointer Devices › Tracking Speed* to taste, and *Devices* to remap the right/middle buttons.

The window shows "iPhone keyboard & mouse connected" when the link is up.

## Using it

- **Click the mirrored screen** to start controlling. The Windows cursor disappears; the mouse and keyboard
  now drive the iPhone. **Ctrl+Alt** (or switching windows) hands them back.
- Left click = tap, right click = AssistiveTouch secondary action, wheel = scroll (tilt wheel = horizontal).
- Keys are sent by physical position, so the iPhone's keyboard layout applies.
- By default **Ctrl ↔ Command** are swapped so Ctrl+C / Ctrl+V / Ctrl+Z work like on Windows; the Windows key
  becomes Control. Setting: `"swap_ctrl_cmd": false` to turn this off.
- Media keys (volume, play/pause, next/previous) control the iPhone.

## Shortcuts

| Keys | Action |
|---|---|
| Ctrl+1 | Home |
| Ctrl+2 | App Switcher (double Home) |
| Ctrl+3 | Spotlight (Command+Space) |
| Ctrl+Alt | Release mouse and keyboard |
| F3 / F11 | Stats overlay / fullscreen |
| Ctrl+Shift+S / Ctrl+Shift+R | Screenshot / start-stop recording |
| Ctrl+Shift+P | Calibrate pointer positioning |

## Pointer calibration (absolute positioning)

AssistiveTouch only accepts relative motion, so by default the iPhone pointer starts wherever it was. After a
one-time calibration, clicking the mirrored screen also **moves the pointer to where you clicked**:

1. With the keyboard/mouse connected, press **Ctrl+Shift+P** in the mirror window.
2. The pointer jumps to the top-left corner and walks diagonally. When it stops, **click exactly on it**
   (Esc cancels).
3. Done — stored as `pointer_cal_x` / `pointer_cal_y`. Recalibrate after changing iOS Tracking Speed.

Positioning moves in small constant steps so iOS pointer acceleration does not distort it; a far jump takes
under a second.

## Tuning

- `"pointer_gain"` (default `1.0`) scales PC mouse counts to iPhone pointer counts; iOS Tracking Speed is the
  other knob.
- The log (`%APPDATA%\PhoneMirror\phonemirror.log`) shows the negotiated Bluetooth connection interval
  (`Control: link interval … ms`) — it bounds pointer latency — and notify timing every 10 s while in use.
