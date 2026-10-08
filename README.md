# MyCam: use your Android phone as a wired USB webcam

Plug the phone into a Windows 11 PC and it shows up as a camera called **MyCam** in Zoom, Teams, OBS,
browsers, and the Windows Camera app. Both the front and back cameras work.

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
| `pc/companion/` | Tray app: AOA handshake (libusb + UsbDk), H.264 decode, frame hand-off. |
| `pc/vcam/` | Virtual camera media source DLL loaded by Windows Frame Server. |
| `pc/common/` | Shared memory layout and the COM CLSID. |
| `pc/install.ps1` / `uninstall.ps1` | One-time PC setup / removal. |
| `pc/tools/capture_test.cpp` | Grabs a frame from the MyCam camera, for testing. |

The wire protocol is defined twice and must stay in sync:
`app/src/main/java/com/example/mycam/Protocol.kt` and `pc/companion/protocol.h`.

## Build

**Android:** `gradlew assembleDebug` (or open the project in Android Studio).

**PC** (Visual Studio 2022 Build Tools + Windows 11 SDK + CMake):
```
cd pc
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Install

1. Install the APK on the phone: `adb install app/build/outputs/apk/debug/app-debug.apk`, or copy it over.
2. On the PC, run once: `powershell -ExecutionPolicy Bypass -File pc\install.ps1` (asks for admin).
   This installs the [UsbDk](https://github.com/daynix/UsbDk) driver (signed by Red Hat; the download is
   hash-checked), copies MyCam to `C:\Program Files\MyCam`, registers the virtual camera, and adds the
   tray app to startup.
3. Plug in the phone. The first time, the phone asks to open MyCam: tick **Always** and tap **OK**,
   then allow camera access.

After that, plugging in is all it takes. Switch cameras from the phone app or the tray menu.

## Testing without a phone

```
"C:\Program Files\MyCam\MyCamCompanion.exe" --test-pattern [--rotate=90]
pc\build\Release\capture_test.exe out.bmp
```

## Known limits

* Windows 11 only (it uses the `MFCreateVirtualCamera` API).
* The phone must support Android Open Accessory. Nearly all phones from the last 10+ years do.
* While the companion is running, any Android phone you plug in switches to webcam (accessory) mode,
  so MTP file transfer isn't available. Exit MyCam from the tray to transfer files.
* `kAccessoryUri` in `pc/companion/protocol.h` is a placeholder. Android shows that link when a phone
  without the app is plugged in, so point it at your download page.
