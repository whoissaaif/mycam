# MyCam: use your Android phone as a wired USB webcam

<p align="center">
  <img src="docs/media/mycam-demo.gif" alt="MyCam demo: plug in your phone and it becomes your webcam" width="800">
</p>

Plug the phone into a Windows 11 PC and it shows up as a camera called **MyCam** in Zoom, Teams, OBS,
browsers, and the Windows Camera app. Both the front and back cameras work.

* **Wired only, plug and play.** No Wi-Fi, and no Developer Options or USB debugging.
* **Privacy first.** Pause from the phone, its notification, or the PC tray. The camera also turns off by itself
  while Windows is locked. Other apps then see a "Camera paused" picture instead of you.
* **Windows 7 Aero look** on both the phone app and the PC (tray icons, settings window, installer).

## How it works

```
Phone (MyCam app)                     USB cable (Android Open Accessory)        PC
Camera2 → H.264 encoder ──────────────────────────────────────────────▶  MyCamCompanion.exe (tray)
         ◀── START / STOP / switch camera ─────────────────────────────    │ decode (Media Foundation)
                                                                           ▼
                                               Global\MyCamFrame shared memory
                                                                           ▼
                           Windows Frame Server ─ MyCamVCam.dll (virtual camera) ─▶ any app
```

* **No Developer Options or USB debugging.** The companion uses Android Open Accessory (AOA) to switch
  the phone into accessory mode. Android then offers to open MyCam; the user ticks "Always" once.
* **Camera only runs while it's in use.** The phone camera turns on when a PC app opens the MyCam
  webcam and turns off a few seconds after the app closes it.
* Portrait and landscape are both handled: the image is rotated upright and letterboxed.

## Layout

| Path | What |
|---|---|
| `app/` | Android app (Kotlin, Compose). `WebcamService` owns the USB link and camera. |
| `pc/companion/` | Tray app: AOA switch (libusb + UsbDk), streaming over WinUSB, H.264 decode, frame hand-off. |
| `pc/vcam/` | Virtual camera media source DLL loaded by Windows Frame Server. |
| `pc/common/` | Shared memory layout and the COM CLSID. |
| `installer/` | Inno Setup installer (`mycam.iss`) and its build script (`build.ps1`). |
| `pc/install.ps1` / `uninstall.ps1` | Developer install/removal without building the installer. |
| `design/` | Design tools (`tools/make_icons.ps1`, `make_installer_art.ps1`) and previews. Style: Windows 7 Aero (IMPROVEMENTS.md section 7). |
| `pc/tests/` | C++ unit tests: packet parser, protocol vectors, frame rotate/scale. |
| `pc/tools/` | Developer tools (not shipped): `capture_test` grabs a MyCam frame, `usb_probe` lists USB devices via UsbDk. |
| `protocol/` | Wire protocol spec (`PROTOCOL.md`) and byte-exact test vectors (`golden.txt`). |

The wire protocol is implemented twice, in `app/.../Protocol.kt` and `pc/companion/protocol.h`. Both test suites
check against `protocol/golden.txt`, so a change on one side alone fails a test. See
[protocol/PROTOCOL.md](protocol/PROTOCOL.md).

## Build

**Android:** `gradlew assembleDebug` (or open the project in Android Studio).

**PC** (Visual Studio 2022 Build Tools + Windows 11 SDK + CMake):
```
cd pc
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # add -DMYCAM_BUILD_TOOLS=ON for the dev tools
cmake --build build --config Release
```

## Tests

```
gradlew :app:testDebugUnitTest                 # Android: protocol encoding/parsing
ctest --test-dir pc/build -C Release           # PC: parser, protocol vectors, frame transform
```

## Install

1. Install the APK on the phone: `adb install app/build/outputs/apk/debug/app-debug.apk`, or copy it over.
2. On the PC, run `MyCam-Setup-1.0.0.exe` (build it with `powershell -ExecutionPolicy Bypass -File installer\build.ps1`;
   output in `installer\output`). It installs the [UsbDk](https://github.com/daynix/UsbDk) driver if needed (signed by
   Red Hat; hash-checked), registers the MyCam camera, and can start MyCam with Windows. Uninstall from
   Settings → Apps.
3. Plug in the phone. The first time, the phone asks to open MyCam: tick **Always** and tap **OK**,
   then allow camera access. Windows also asks for admin once per new phone, to give its accessory mode
   the built-in WinUSB driver.

After that, plugging in is all it takes. Left-click the tray icon for the settings window (camera, mirror,
start with Windows); right-click for the quick menu.

## Testing without a phone

```
"C:\Program Files\MyCam\MyCamCompanion.exe" --test-pattern [--rotate=90]
pc\build\Release\capture_test.exe out.bmp      # needs -DMYCAM_BUILD_TOOLS=ON
```

## Troubleshooting

The companion writes `%LOCALAPPDATA%\MyCam\mycam.log`, including the phone's own messages ("phone says: ...").
It records each step: phone found, switched to accessory mode, connected, camera started, frames decoded.

Exit MyCam from the tray rather than killing it. A killed companion can leave the phone stuck until it's
replugged.

## Known limits

* Windows 11 only (it uses the `MFCreateVirtualCamera` API).
* The phone must support Android Open Accessory. Nearly all phones from the last 10+ years do.
* While the companion is running, any Android phone you plug in switches to webcam (accessory) mode,
  so MTP file transfer isn't available. Exit MyCam from the tray to transfer files.

## License

MIT. See [LICENSE](LICENSE). Third-party parts keep their own licenses:
[libusb](https://libusb.info) (LGPL-2.1, vendored in `pc/third_party/libusb`),
[UsbDk](https://github.com/daynix/UsbDk) (Apache-2.0, downloaded by the installer build), and
[Selawik](https://github.com/microsoft/Selawik) (SIL OFL 1.1). The Windows 7 look is an homage drawn from scratch;
no Microsoft artwork is included.
