# MyCam: task tracker

Working list. The detail behind each item is in [IMPROVEMENTS.md](IMPROVEMENTS.md) (section numbers in
brackets). Move items between sections as they progress, and record releases and decisions at the bottom.

Status: `[ ]` to do · `[~]` in progress · `[x]` done

---

## Now

- [ ] Install 1.2.0: on the phone, uninstall the old `com.example.mycam` app once, then install 1.2.0
- [ ] Test the remaining scenarios [2.3]
  - [ ] Pause / Resume from the phone's notification
  - [ ] Pause survives unplug and replug
  - [ ] PC sleep and resume while streaming
  - [ ] Companion crash or kill mid-stream, then restart
  - [ ] A phone that has never been connected (first-use prompts)
  - [ ] Two phones plugged in at once
  - [ ] Phone low-battery / battery-saver mode

## Next

- [ ] Recover when the UsbDk fallback leaves the phone hidden [2.1]
- [ ] One UAC prompt for a new phone instead of two [2.2]
- [ ] Crop to fill (no black bars for a portrait phone) [3.1]
- [ ] Dim the phone screen while streaming, with a heat warning [4.1]
- [ ] Write `design/STYLE.md` [7.2]

## Backlog

- [ ] Resolution and frame-rate options (720p / 1080p / 4K, 60 fps) [3.2]
- [ ] Camera controls: zoom, focus, exposure, torch, lens choice [3.3]
- [ ] Lower latency: GPU decode, fewer copies, latency measurement [3.4]
- [ ] Screen-reader support for the settings window (UI Automation) [5.2]
- [ ] Code-sign the installer and companion [5.3]
- [ ] Decide public vs. private repo before sharing builds [8]
- [ ] Play Store assets (512 px icon, feature graphic), if publishing there [7.3]
- [ ] iPhone support: iOS app + usbmux transport on the PC (future; needs a Mac, an iPhone, Apple Developer account) [9]

## Done

- [x] Package name `io.github.whoissaaif.mycam`, release key created, signed release [4.2] (1.2.0)
- [x] Ad brief for the motion designer (shared doc)
- [x] Wired phone-as-webcam: Android Open Accessory + Windows 11 virtual camera (1.0)
- [x] Fixes from first real-phone testing: MediaTek config, WinUSB streaming, reliable reads, logging (1.0)
- [x] Project hygiene: git, unit tests, protocol spec and golden vectors [6]
- [x] Design language decided: Windows 7 Aero [7.1]
- [x] PC app: tray status icons, Aero settings window, Inno Setup installer [5.1–5.3]
- [x] Pause on phone and PC, "Camera paused" picture [1.1] (1.1.0)
- [x] Auto-pause while Windows is locked or asleep [1.2] (1.1.0)
- [x] Aero phone app and launcher icon [1.3] (1.1.0)
- [x] Published to GitHub (private), email hidden, release v1.1.1 [8]

---

## Releases

| Version | Date | What changed |
|---|---|---|
| 1.2.0 | 2026-10-09 | Package `io.github.whoissaaif.mycam` (new app on phones), signed with the release key, 1.5 MB APK. |
| 1.1.1 | 2026-10-09 | The phone's "get the app" link points to the GitHub repo. First GitHub release (exe + apk). |
| 1.1.0 | 2026-10-09 | Pause on phone and PC, auto-pause on Windows lock, status pictures, Aero phone app. |
| 1.0.0 | 2026-10-09 | First installer: tray status icons, Aero settings window, Inno Setup. |

Release steps: bump the versions (`app/build.gradle.kts`, `installer/mycam.iss`, `installer/build.ps1`),
run `installer\build.ps1` and `gradlew assembleDebug`, commit, tag `vX.Y.Z`, push, then
`gh release create vX.Y.Z <exe> <apk>`.

## Decisions

| Date | Decision |
|---|---|
| 2026-10-09 | Transport: Android Open Accessory (no Developer Options) + Windows 11 virtual camera |
| 2026-10-09 | UsbDk only switches the phone into accessory mode; streaming uses Windows' WinUSB driver |
| 2026-10-09 | Design language: Windows 7 Aero (option A) |
| 2026-10-09 | The phone owns the pause state; Windows-lock pause is PC-only and never undoes a user pause |
| 2026-10-09 | License: MIT. Repo private on `whoissaaif`. `inspo/` (Microsoft artwork) never committed |
| 2026-10-09 | Commits use the GitHub no-reply email |
