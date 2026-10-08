# MyCam: improvement backlog

Effort: **S** = small (hours), **M** = medium (a day or two), **L** = large (a week or more).

## Field test results (2026-10-09)

| Scenario | Result | Follow-up |
|---|---|---|
| Long video call | ✅ Uninterrupted | Add a pause button on the phone (see 1.1) |
| Phone screen locked while streaming | ✅ Uninterrupted | None |
| Windows locked while streaming | ⚠️ Keeps streaming | **Privacy issue:** must pause (see 1.2) |
| Unplug and replug | ✅ Reconnects instantly | None |
| Opening the phone's own camera app | ✅ Stream stops | Correct behavior. **Do not change.** |

Still untested: PC sleep and resume, killing or crashing the companion mid-stream, two phones plugged in at
once, and a phone that has never been connected before (the first-use UAC flow).

---

## 1. Privacy and control (highest priority)

### 1.1 Pause button on the phone (S)
A **Pause / Resume** button in the app and in the notification. While paused, the phone camera is off and
the PC shows a "Paused" frame instead of black, so the person on the other end of a call knows it's
intentional.
- Phone: new `TYPE_STATE` value `STATE_PAUSED`. The phone ignores `CMD_START` while paused.
- PC: set `phoneState = kPhonePaused` in shared memory. The virtual camera renders a paused frame and the
  tray shows "Paused".

### 1.2 Pause when Windows is locked (S): privacy fix
Stop the phone camera when the Windows session is locked, and resume on unlock.
- Companion: `WTSRegisterSessionNotification`, then `WTS_SESSION_LOCK` / `WTS_SESSION_UNLOCK` → send
  `CMD_STOP` / allow `CMD_START`.
- Also pause on sleep (`WM_POWERBROADCAST`), and when the user switches Windows accounts.
- The virtual camera shows the paused frame while locked.

---

## 2. Reliability

### 2.1 Recover from a force-kill or crash (S)
If the companion dies while UsbDk holds the phone, the phone stays hidden until it is replugged. On start,
detect an accessory that can't be opened and reset it (`UsbDkController`-style reset, or bounce the port),
or tell the user to replug.

### 2.2 Fewer UAC prompts on first use (S)
A new phone currently prompts twice: once at install, then again for WinUSB binding. Have `install.ps1`
bind any phone that is plugged in, and explain the one remaining prompt in the tray balloon.

### 2.3 Test the remaining scenarios (S–M)
PC sleep and resume, two phones at once, phone low-battery mode, and Android killing the service under
memory pressure. Fix whatever breaks.

---

## 3. Picture quality and camera control

### 3.1 Crop to fill (S)
A portrait phone currently gives a narrow image with black bars. Add a "Crop to fill" option that
center-crops to 16:9 (in `FrameReader::Render`), with a tray toggle.

### 3.2 Resolution and frame-rate options (M)
720p / 1080p / 4K, and 60 fps where the phone supports it. Add a protocol command to request a mode, and
advertise the matching media types from the virtual camera.

### 3.3 Camera controls (M)
Zoom, tap-to-focus, exposure, torch, and lens choice (ultrawide / telephoto, not just front and back).
Controls go on the phone UI first; exposing them to PC apps through `IKsControl` is a stretch goal.

### 3.4 Lower latency (M)
Decode on the GPU (D3D11 / DXVA) instead of the CPU, cut extra frame copies, and add a latency
measurement (timestamp in the frame header → log).

---

## 4. Phone app

### 4.1 Dim the screen while streaming (S)
A black "streaming" screen keeps the phone cooler, saves battery and is more private. Show a warning when
the phone gets hot (`PowerManager.getCurrentThermalStatus`).

### 4.2 Branding (S)
A real app icon, and a proper package name instead of `com.example.mycam`.

---

## 5. PC app

### 5.1 Tray icon that shows status (S)
A different icon or badge for disconnected, ready, streaming and paused, instead of one icon with a
tooltip.

### 5.2 Settings window (M)
Camera, resolution, mirror, crop, and start with Windows, all in one small window.

### 5.3 Real installer (M)
An `.exe` or MSI (Inno Setup / WiX) instead of `install.ps1`, plus code signing to remove the "unknown
publisher" warnings and an uninstaller entry in Settings → Apps.

---

## 6. Project hygiene (S): ✅ done
- ✅ Git repo set up and committed.
- ✅ `capture_test` and `usb_probe` are behind `-DMYCAM_BUILD_TOOLS=ON` and never shipped.
- ✅ Unit tests: C++ (`pc/tests`, packet parser and frame transform) and Android (`ProtocolTest`).
- ✅ Protocol spec in `protocol/PROTOCOL.md`; both sides tested against `protocol/golden.txt`.
- Found by the tests and fixed: a stray `MCAM`/`MCMD` inside garbage could be taken for a header,
  and the phone's command reader could crash on a read over 4 KiB.

---

## Suggested order

1. **1.2** Pause when Windows is locked (privacy)
2. **1.1** Pause button on the phone
3. **3.1** Crop to fill, and **4.1** dim the screen while streaming
4. **2.1–2.3** Remaining reliability work
5. Everything else as needed
