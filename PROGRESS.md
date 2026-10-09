# MyCam: progress log

How the project got from an idea to v1.3.1, what was learned on the way, and where it stands now.
[TASKS.md](TASKS.md) is the live to-do list. [IMPROVEMENTS.md](IMPROVEMENTS.md) holds the detail behind each item.

---

## Where things stand (2026-10-10)

- **Latest release: v1.3.1** (private): https://github.com/whoissaaif/mycam/releases/tag/v1.3.1
  (`MyCam-Setup-1.3.1.exe` + `MyCam-1.3.1.apk`). The PC and the test phone both run 1.3.1.
- **Working:** plug-and-play streaming; front/back camera; pause from the phone or the PC; auto-pause when
  Windows is locked or asleep; instant reconnect after a replug; 720p/1080p/4K; camera controls (zoom,
  brightness, focus lock, torch, Auto reset); crop to fill; adaptive bitrate; Aero design on both sides;
  installer; clean install verified.
- **Uncommitted:** `WebcamService.kt` has a fix to adaptive streaming. The bitrate maths now uses `Long`,
  because 4K bitrate × 115 overflowed `Int`, sent the encoder a negative bitrate and collapsed the picture
  about every 30 s. It also adds a ceiling of 90% of the bitrate at which the link last stalled. The fix
  still needs a test on the phone, a commit and a release (1.3.2).
- **Biggest open issue:** phone-side latency. Capture → encoded takes **150–230 ms** on the test phone,
  compared with 3–12 ms for decoding and copying on the PC.

---

## Timeline

### 1. Idea and architecture
The goal was an Android app that makes the phone a **wired-only, plug-and-play** webcam with front and back
cameras.
- Transport: **Android Open Accessory**. It needs no Developer Options and no ADB, and the phone shows one
  permission prompt.
- PC side: a **native Windows 11 virtual camera** (`MFCreateVirtualCamera` + a custom media source), so every
  app sees "MyCam" without a third-party driver.
- Pipeline: Camera2 → MediaCodec H.264 → USB → Media Foundation decoder → shared memory → virtual camera.

### 2. First real-phone testing (1.0 fixes)
| Symptom | Cause | Fix |
|---|---|---|
| "USB driver missing, run install.ps1" | UsbDk not installed / libusb couldn't use it | Init the libusb context first, then set the UsbDk option |
| "Plug in your phone" stayed grey; no camera prompt | The accessory-mode device had no driver, which broke UsbDk enumeration | Elevated `--bind-driver` installs inbox WinUSB; stream through a WinUSB libusb context |
| Phone camera in use (green dot) but black picture | The MediaTek encoder never sends `CODEC_CONFIG`; timed-out reads dropped data | Config from `onOutputFormatChanged`; queued async reads; queued keyframe requests |
| Phone kept streaming after the companion was killed | No session ownership | STOP when the phone streams without being asked |

User testing confirmed the stream survives calls and the phone screen locking, and reconnects instantly after
a replug. Opening the phone's own camera app stops the stream, and the user confirmed that's the correct
behaviour.

### 3. Planning and hygiene
- Wrote **IMPROVEMENTS.md** (later trimmed: no microphone, no live preview, no mirrored front camera by default).
- Project hygiene: git, unit tests on both sides (116 PC checks), protocol spec and golden vectors.
- **Design language: Windows 7 Aero (option A)**, taken from the user's `inspo/` folder (never committed).

### 4. PC app (1.0.0)
Aero tray status icons, a custom Direct2D settings window, an Inno Setup installer (bundles UsbDk and
offers to remove it on uninstall) and a per-monitor DPI manifest.

### 5. Privacy and control (1.1.0)
- Pause from the phone **and** from the PC. The phone owns the pause state, which persists. The virtual
  camera shows a "Camera paused" picture.
- Auto-pause while Windows is locked or asleep (PC-only, and it never undoes a user pause).
- Aero redesign of the phone app: Selawik font, glass header, command links and a new launcher icon.

### 6. GitHub (1.1.1)
Published to `whoissaaif/mycam` under MIT, then switched to **private**. Commits use the no-reply email, and
`inspo/` was kept out of history. The first release had both the exe and the apk. The README shows the demo
GIF; the screenshots were removed.

### 7. Branding and signing (1.2.0)
- Package renamed to `io.github.whoissaaif.mycam`, so it installs as a new app on phones.
- Release keystore at `%USERPROFILE%\.mycam\` (the user entered the password), with signed release builds.
  The APK is 1.5 MB.
- An ad brief for a motion-designer friend was written as a shared doc.
- iPhone support was recorded as future section 9 (needs a Mac, an iPhone, a developer account and usbmux).

### 8. Clean install test
Everything was removed and the PC rebooted, then 1.2.0 was installed from the GitHub release. The first-use
flow and streaming worked. The only runtime dependencies are the bundled UsbDk and Windows' own DLLs. Windows
Sandbox couldn't be used because CPU virtualization is off in the firmware.

### 9. Picture quality and camera control (1.3.0 → 1.3.1, protocol v3)
- 720p / 1080p / 4K and 30 / 60 fps, with options the phone can't do disabled. The virtual camera also
  offers 4K30, 1080p60 and 720p60.
- Zoom, brightness (EV), focus once-and-lock, and torch, applied live from the phone or the PC.
- **Auto** button (user request) resets all camera controls in one click.
- Crop to fill instead of letterboxing.
- **Adaptive streaming:** a slow USB write lowers the bitrate by 25% and skips to the next keyframe. The
  bitrate climbs back by 15% every 5 s while writes stay fast. Added after 4K showed large latency.
- PC latency: frames are delivered to apps as they arrive, with one less full-frame copy.
- **60 fps investigation:** the user saw "1080p 60" in the phone's own camera app. A camera report added to
  the log shows both cameras max out at 30 fps at 1080p in Camera2's normal modes. This was first misread as
  "no 60 fps at all". On 2026-10-10 the same report showed the back camera has a high-speed mode (1080p at
  120/240 fps), and 1.3.2 uses it for 1080p60 (capture at 120, encode every other frame).

---

## Lessons learned

- **USB on Windows:** UsbDk is good for the AOA switch but can't take over a driver-bound device. WinUSB
  through SetupAPI works and is Microsoft-signed.
- **MediaTek encoders** don't always follow the MediaCodec contract (no `CODEC_CONFIG`; frames are held back,
  which is the likely cause of the phone-side latency).
- **Measure before optimising:** latency logging on both clocks showed the PC isn't the bottleneck.
- **Tooling on Windows:** PowerShell 5 corrupts UTF-8 and Perl escapes misbehave. Use the Edit tool or sed.
  `git filter-branch` can delete untracked working-tree files (inspo was restored from `refs/original`).
- **GitHub licence detection** needs the plain MIT text, with nothing added.
- **Installer upgrades** must call `--quit` on the new exe, not the installed one.
- **Integer overflow** in bitrate maths at 4K (fixed, not yet committed).

## Next up (see TASKS.md)

1. Test, commit and release the adaptive-bitrate overflow fix (1.3.2).
2. Lower phone-side latency: vendor low-latency encoder keys, fewer camera buffers, keyframe spacing.
3. Remaining test scenarios: notification pause, pause across a replug, PC sleep, companion crash, two
   phones, battery saver.
4. Reliability: recover when the UsbDk fallback leaves the phone hidden; one UAC prompt instead of two.
5. Dim the phone screen while streaming; write `design/STYLE.md`; code-sign; full UsbDk removal on uninstall.
