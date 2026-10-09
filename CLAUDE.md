# CLAUDE.md

Guidance for Claude Code sessions in this repo. Read [PROGRESS.md](PROGRESS.md) for history,
[TASKS.md](TASKS.md) for what's next, and [IMPROVEMENTS.md](IMPROVEMENTS.md) for the reasoning behind each item.

## What this is

MyCam turns an Android phone into a **wired** USB webcam for **Windows 11**. It's plug and play: the phone
asks for permission once, and the PC shows one UAC prompt per new phone. Front and back cameras are supported.

- **Android app** (`app/`, Kotlin + Jetpack Compose). Camera2 → MediaCodec H.264 (surface input) → USB
  through **Android Open Accessory (AOA)**. No Developer Options or ADB needed.
- **Windows companion** (`pc/companion/`, C++17, Win32 tray app). It switches the phone into accessory mode,
  reads the stream, decodes it with the Media Foundation H.264 MFT and writes NV12 frames to shared memory.
- **Virtual camera** (`pc/vcam/`, COM DLL). It's a custom `IMFMediaSourceEx` that Windows Frame Server loads
  through `MFCreateVirtualCamera`. It reads frames from shared memory `Global\MyCamFrame` (the DLL creates it
  inside the service and the companion opens it; a seqlock header is in `pc/common/shared_frame.h`).
- **Protocol** (`protocol/PROTOCOL.md`, currently **v3**). It's big-endian. Phone→PC packets have a 20-byte
  `MCAM` header; PC→phone commands are 8 bytes and start with `MCMD`. Golden vectors in `protocol/golden.txt`
  are tested by **both** sides. If the protocol changes, update the spec, the golden file, `Protocol.kt` and
  `pc/companion/protocol.h` together, and bump the version.

## USB transport (hard-won; don't undo)

- **UsbDk** is used **only** to send the AOA switch while Windows' MTP driver owns the phone.
- After the switch, the accessory device has no driver. The companion binds Windows' inbox **WinUSB**
  (`winusb.inf`) through an elevated `MyCamCompanion.exe --bind-driver` (SetupAPI, `winusb_bind.cpp`).
  Streaming then uses a **separate libusb context with the WinUSB backend**, because UsbDk can't redirect a
  driver-bound device.
- Since 1.3.2 the installer runs `--register-task`, which creates the SYSTEM scheduled task "MyCam phone
  driver" (`driver_task.cpp`). Authenticated users may only start it, and it only runs `--bind-driver`. The
  companion starts that task instead of asking for UAC, and falls back to UAC if the task is missing or
  points at a different exe. The user approved this design on 2026-10-10; don't widen what the task does.
- With UsbDk, call `libusb_init_context` first and then `libusb_set_option(USE_USBDK)`. Passing it as an init
  option fails.
- Reads are queued async transfers. Timed-out sync reads lose data. Don't send synchronously from inside a
  transfer callback (it returns BUSY); queue the command instead.
- If the phone streams without being asked (for example, after the companion was killed), send STOP.
- Some MediaTek encoders never emit a `CODEC_CONFIG` buffer. In that case, take SPS/PPS from
  `onOutputFormatChanged`. The PC can initialise the decoder without config.

## Build and test

Android (JDK from Android Studio; minSdk 24, targetSdk 37):
```
gradlew assembleDebug            # debug APK
gradlew assembleRelease          # signed if keystore.properties exists (git-ignored)
gradlew testDebugUnitTest        # ProtocolTest incl. golden vectors
gradlew lintDebug
```

PC (Visual Studio 2022 + CMake 3.20+; static CRT, `/utf-8`):
```
cmake -S pc -B pc/build -A x64 && cmake --build pc/build --config Release
ctest --test-dir pc/build -C Release      # mycam_tests (protocol, parser, golden, transforms)
```
Dev install without the installer: `pc\install.ps1` / `pc\uninstall.ps1` (elevated).

Installer: `powershell -ExecutionPolicy Bypass -File installer\build.ps1 -Version X.Y.Z`. It builds
`pc/build-release`, runs the tests, fetches UsbDk (hash-checked) and runs Inno Setup 6 →
`installer/output/MyCam-Setup-X.Y.Z.exe`.

Release steps: bump the versions in `app/build.gradle.kts` (versionCode + versionName),
`installer/mycam.iss` and `installer/build.ps1`. Build the installer and the APK, commit, tag `vX.Y.Z`,
push, then run `gh release create vX.Y.Z <exe> <apk> --notes-file ...`. Add a row to the Releases table in
TASKS.md.

## Rules from the user

- **Repo is private** (`github.com/whoissaaif/mycam`). Don't make it public without asking.
- **Never commit `inspo/`** (it contains Microsoft artwork), `screenshots/`, `release/`, `keystore.properties`
  or `*.jks`. The release keystore lives at `%USERPROFILE%\.mycam\mycam-release.jks`. **The user types the key
  password themselves.** Never ask for it, store it or echo it.
- Commits use the GitHub **no-reply email**, with author name `saif`. Don't expose the real email.
- Don't push, tag or release unless asked. "Keep it local" means no pushing.
- Package / applicationId: `io.github.whoissaaif.mycam`. Changing it makes a new app on phones.
- Keep phone behaviour that the user confirmed is correct: opening the phone's own camera app stops the stream.
- Track work in TASKS.md (status + Releases + Decisions tables) and IMPROVEMENTS.md (section numbers
  are referenced from TASKS.md). Update both when something lands.

## Wireless (branch `wireless`, 1.4.0-beta)

- **Phone:** `WirelessServer.kt` (UDP discovery on 47801, TCP on 47800), `WifiHandshake.kt` (pairing and
  proof, `PairedPcs` storage), `WifiCrypto.kt` (ECDH, HKDF, AES-GCM, `SecureInput/OutputStream`).
  `WebcamService` runs a generic link: a USB accessory or a socket.
- **PC:** `phone_link_wifi.cpp` (discovery, handshake, encrypted records, DPAPI key storage in
  `HKCU\Software\MyCam\PairedPhones`) and `wifi_crypto.cpp` (CNG). One single-threaded `SessionLoop` serves
  both USB and Wi-Fi. Keep it that way: Aiyan's two-thread version deadlocks.
- The wire format is specified in PROTOCOL.md "Wireless transport" / "Wireless security". The `wifi.*`
  vectors in golden.txt were generated with the JDK (the generator is a one-off, not in the repo). Both test
  suites check them.
- **Testing Wi-Fi:** unplug the cable, because a plugged-in phone always takes over, and use wireless adb.
  adb on USB blocks MyCam's USB access.

## Releasing

- Test the **release** APK on a real phone before publishing: R8 only runs on release builds (1.3.2
  crashed on launch because of R8's `packageScope`, now removed).
- After `MyCamCompanion.exe --quit`, wait for the process to exit before starting it again (single instance).

## Works on every phone

MyCam must not be tuned to the test phone. Every option in either UI comes from what the connected phone
reports, for each combination. For example, 60 and 120 fps depend on the quality: many phones only do 30
at 4K.
- `CameraStreamer.modeFor(camera, quality, fps)` is the single place that decides what works. It checks the
  normal modes, the high-speed modes, and the encoder at that exact size. Camera start-up and the CAMERA
  frame-rate masks both use it.
- Vendor tweaks (such as the MediaTek encoder keys) are found at runtime, are optional, and have a generic
  fallback.

## Design language: Windows 7 Aero

Both apps use a Win7 Aero look: glass header, glossy buttons, command links and the Selawik font (OFL,
licence in `app/src/main/assets/licenses`). Phone components are in `app/.../ui/aero/AeroComponents.kt` and
the tokens in `ui/theme/`. The PC settings window is custom Direct2D/DirectWrite (`settings_window.cpp`).
Art is generated by scripts in `design/tools/*.ps1`; regenerate with them rather than hand-editing PNGs.

## Windows / tooling gotchas

- The shell is Windows. PowerShell 5's `Get-Content`/`Set-Content` mangle UTF-8 (sources contain `·`, `×`,
  `→`). Use the Edit tool, or bash `sed`, for files with non-ASCII text. PS5 also misreads BOM-less UTF-8
  scripts.
- Avoid Perl one-liners for edits (`\a`, `\c` and `${}` interpolation have bitten before).
- In .NET calls from PowerShell, use absolute paths (the .NET cwd differs from `$PWD`).
- During installer upgrades, `--quit` must run the **new** exe, which `PrepareToInstall` extracts to `{tmp}`.
  The old one can hang setup.
- The virtual camera is per session/user. If a change to the DLL doesn't show up, the Frame Server service
  is still holding the old one: reinstall, or restart the companion and the camera app.
- Windows Sandbox / Hyper-V aren't available on this PC (CPU virtualization is off in the firmware).

## Diagnostics

- Companion log: `%LOCALAPPDATA%\MyCam\` (tray → settings → "Open log folder"). Phone `LOG` packets
  (camera report, latency, adaptive-bitrate changes) are copied into it.
- `MyCamCompanion.exe --test-pattern` streams a synthetic picture without a phone. Developer tools
  (`capture_test`, `usb_probe`) are built with `-DMYCAM_BUILD_TOOLS=ON`.
- The test phone is a MediaTek device. Its normal Camera2 modes stop at 30 fps, but the back camera has a
  constrained high-speed mode (1080p/720p at 120 and 240 fps). 1080p60 uses that mode at 120 fps, and the
  encoder drops every other frame. The front camera can only do 30.
