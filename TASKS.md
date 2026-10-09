# MyCam: task tracker

Working list. The detail behind each item is in [IMPROVEMENTS.md](IMPROVEMENTS.md) (section numbers in
brackets). Move items between sections as they progress, and record releases and decisions at the bottom.

Status: `[ ]` to do · `[~]` in progress · `[x]` done

---

## Now

- [x] Clean install test: removed everything, installed 1.2.0 from the GitHub release, first-use flow and streaming work
- [ ] Test the remaining scenarios [2.3]
  - [ ] Pause / Resume from the phone's notification
  - [ ] Pause survives unplug and replug
  - [ ] PC sleep and resume while streaming
  - [ ] Companion crash or kill mid-stream, then restart
  - [x] A phone that has never been connected (first-use prompts): passed in the clean install test
  - [ ] Two phones plugged in at once
  - [ ] Phone low-battery / battery-saver mode

## Next

- [ ] Recover when the UsbDk fallback leaves the phone hidden [2.1]
- [~] No UAC prompt for new phones (SYSTEM scheduled task from the installer) [2.2]: built and installed (1.3.2), test with a never-connected phone

- [~] Dim the phone screen while streaming, with a heat warning [4.1]: built, test on the phone
- [ ] Write `design/STYLE.md` [7.2]

## Backlog

- [~] Lower phone-side latency (capture→encoded 150–230 ms on the test phone) [3.4]: camera/encoder tweaks and a camera-vs-encoder split in the log built; measure on the phone
- [~] 30 / 60 / 120 fps choice; 60 and 120 on the back camera through high-speed capture [3.2]: built, test on the phone
- [ ] Adaptive-bitrate Int overflow fix at 4K (in `WebcamService.kt`): test, then release as 1.3.2
- [ ] Screen-reader support for the settings window (UI Automation) [5.2]
- [ ] Code-sign the installer and companion [5.3]
- [ ] Uninstaller: remove UsbDk completely (service + UsbDk.sys stay behind after msiexec) [5.3]
- [ ] Test a clean install on a different PC (Sandbox needs CPU virtualization enabled in firmware) [2.3]
- [ ] Decide public vs. private repo before sharing builds [8]
- [ ] iPhone support: iOS app + usbmux transport on the PC (future; needs a Mac, an iPhone, Apple Developer account) [9]

## Done

- [x] Assets complete: Play Store 512 px icon + 1024×500 feature graphic, dimmed streaming screen [7.3]

- [x] Picture quality and camera control: crop to fill, 720p/1080p/4K, 60 fps where supported, zoom/brightness/focus/torch + Auto, adaptive streaming [3] (1.3.1)

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
| 1.3.2 | 2026-10-10 | 30/60/120 fps per quality (high-speed capture), no-UAC phone setup, dim screen + heat warning, lower phone latency, 4K bitrate fixes. |
| 1.3.1 | 2026-10-09 | Quality & 60 fps, camera controls + Auto, crop to fill, adaptive streaming, lower PC latency (protocol v3). |
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
