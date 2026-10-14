# MyCam: use your phone as a webcam

<p align="center">
  <a href="https://github.com/whoissaaif/mycam/releases/latest"><img src="https://img.shields.io/github/v/release/whoissaaif/mycam" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/whoissaaif/mycam" alt="License: MIT"></a>
</p>

<p align="center">
  <img src="docs/media/mycam-demo.gif" alt="MyCam demo: plug in your phone and it becomes your webcam" width="800">
</p>

Turn your phone into a camera called **MyCam** on a Windows 11 PC. It shows up in Zoom, Teams, OBS,
browsers, and the Windows Camera app. Both the front and back cameras work.

* **Android:** plug and play over a USB cable, or over Wi-Fi after a one-time pairing. You don't need
  Developer Options or USB debugging.
* **iPhone and iPad:** a page in Safari, served by the PC itself. Scan a QR code and there's nothing to
  install on the phone. See [iPhone](#iphone).
* **Privacy first.**
  * Pause from the phone, its notification, or the PC tray.
  * The camera also turns off by itself while Windows is locked.
  * While paused, other apps see a "Camera paused" picture instead of you.
* **Windows XP look** on the phone app, the iPhone page and the PC (tray icons, the MyCam window with a
  live preview, the installer), with a friendly camera logo.

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

* **No Developer Options or USB debugging.**
  * The companion uses Android Open Accessory (AOA) to switch the phone into accessory mode.
  * Android then offers to open MyCam; the user ticks "Always" once.
* **The camera only runs while it's in use.** The phone camera turns on when a PC app opens the MyCam
  webcam, and turns off a few seconds after the app closes it.
* Portrait and landscape are both handled: the image is rotated upright and letterboxed.
* **Wi-Fi (Android)** carries the same packets over an encrypted TCP link after a one-time pairing.
  See "Wireless transport" in [PROTOCOL.md](protocol/PROTOCOL.md).
* **One phone at a time.** A phone on the cable always wins over Wi-Fi and the iPhone.

## iPhone

An iPhone has no app. The companion runs a small HTTPS server, and Safari opens the MyCam page from it.
The page encodes H.264 on the iPhone's hardware (WebCodecs) and sends the **same MyCam packets** as
Android, one per WebSocket message. From the decoder onward, the PC treats it exactly like an Android
phone.

### How it fits together

```
iPhone (Safari, MyCam page)                    Wi-Fi / LAN, TCP 47900                 PC
getUserMedia (camera) ─▶ WebCodecs H.264 ──── wss:// (TLS, self-signed) ─────▶  MyCamCompanion.exe
motion sensors ─▶ ORIENT (which way is up)      one MyCam packet per message       web_server.cpp (Schannel)
                 ◀── START / STOP / switch camera / quality (MCMD) ──────────       │ phone_link_web.cpp
                                                                                    ▼
                                                     same SessionLoop as USB and Android Wi-Fi
                                                                                    ▼
                                         decode → Global\MyCamFrame → MyCamVCam.dll → any app
```

### Pairing (first time)

```
  PC (Connect an iPhone page)          iPhone                                Companion server
  ─────────────────────────            ──────                                ────────────────
  shows a QR code with a        ──▶    Camera app scans it
  one-time code (5 min, once)          Safari: "not private" → visit once ─▶ GET /  (the page)
                                       page: POST /pair  code + name   ────▶ checks the code, uses it up
                                       stores the device token        ◀──── new device token
                                                                              (PC keeps only its SHA-256)
  later visits:                        page: POST /check  token        ────▶ known device?
                                       Start → allow camera
                                       wss://…/ws?d=token              ────▶ token + Origin checked
                                       HELLO, CONFIG, FRAMEs           ────▶ streams when an app opens MyCam
```

* The QR code holds a **one-time code**: 128-bit, valid for 5 minutes, usable once. The page trades it
  for a **device token** (256-bit) and keeps that token in the browser.
* The PC stores only the token's SHA-256 (`HKCU\Software\MyCam\WebDevices`). **Forget** (on the
  Connect an iPhone page or the Devices page) removes it, and ends the iPhone's session.
* The certificate is self-signed. It's made once per Windows user and kept in `CurrentUser\My` for
  397 days, then renewed by itself. Safari asks you to tap through its warning once.
* The installer opens TCP 47900 in Windows Firewall for **private networks only**.

### Setup

1. Install MyCam on the PC (see [Install](#install)). The installer adds the firewall rule.
2. Put the iPhone and the PC on the **same network**, and make sure Windows calls that network
   **Private**.
3. On the PC, open MyCam (left-click the tray icon) and choose **Connect an iPhone**. A QR code appears.
   If the PC is on several networks, pick the one the iPhone is on.
4. On the iPhone:
   1. Open the Camera app, point it at the code, and tap the link.
   2. The first time, Safari says the connection isn't private. Tap **Show Details**, then **visit this
      website**.
   3. Tap **Start**, then allow the camera and the motion sensors.
5. Choose **MyCam** as the camera in any app on the PC.

The iPhone is remembered after that. Open the page again from history, or use **Share → Add to Home
Screen**, and there's no QR code to scan.

On the page:
* **Pause** turns the camera off.
* **Back / Front** switches the camera.
* **Dim the screen** saves battery.

Resolution, frame rate, zoom and torch are chosen on the PC, from what the iPhone reports it can do.

### Using it

* **Keep the page open, in front.** If the iPhone locks or you switch apps, the camera pauses; apps on
  the PC see "Camera paused". The PC keeps the session for **5 minutes**, and the camera resumes when you
  come back to the page.
* **Rotation:** turn the iPhone any way, including upside down. The picture stays upright for the
  viewer.
* **Plugging in an Android phone** takes over. The iPhone page says the PC is busy, and it comes back by
  itself when the cable is unplugged.

### Wired iPhone

Not supported yet. The groundwork and the plan (using the iPhone's USB Personal Hotspot) are in
[docs/iphone-wired.md](docs/iphone-wired.md).

### More detail

* [docs/web-camera-format.md](docs/web-camera-format.md): the server, its routes, the certificate,
  tokens, the messages, rotation, and how it's tested.
* [docs/AIYAN-IPHONE.md](docs/AIYAN-IPHONE.md): the assignment and the definition of done.

## Layout

| Path | What |
|---|---|
| `app/` | Android app (Kotlin, Compose). `WebcamService` owns the link (USB or Wi-Fi) and the camera. |
| `pc/companion/` | Tray app: the AOA switch (libusb + UsbDk), streaming over WinUSB, Wi-Fi, the iPhone server, H.264 decode, the frame hand-off. |
| `pc/companion/web/` | The iPhone page (`index.html`, `app.js`, `protocol.js`, `style.css`), embedded in the exe through `companion.rc`. |
| `pc/companion/web_server.cpp`, `tls_server.cpp`, `web_proto.cpp`, `phone_link_web.cpp` | The iPhone server: HTTPS + WebSocket, the Schannel TLS and certificate, the pure parsing and token code (unit-tested), and the transport in `SessionLoop`. |
| `pc/vcam/` | Virtual camera media source DLL loaded by Windows Frame Server. |
| `pc/common/` | Shared memory layout and the COM CLSID. |
| `pc/third_party/` | Vendored libusb, and [qrcodegen](https://www.nayuki.io/page/qr-code-generator-library) for the iPhone QR code. |
| `installer/` | Inno Setup installer (`mycam.iss`) and its build script (`build.ps1`). |
| `pc/install.ps1` / `uninstall.ps1` | Developer install/removal without building the installer. |
| `design/` | Art tools (`tools/*.ps1`) and previews. Style: Windows XP Luna Blue ([design/STYLE.md](design/STYLE.md)). |
| `pc/tests/` | C++ unit tests: packet parser, protocol vectors, frame rotate/scale, WebSocket frames, tokens. |
| `pc/tools/` | Developer tools (not shipped): `capture_test`, `usb_probe`, `web_probe`, and the iPhone page checks in `webtest/`. |
| `protocol/` | Wire protocol spec (`PROTOCOL.md`) and byte-exact test vectors (`golden.txt`). |
| `docs/` | Design notes, including the iPhone format and the wired-iPhone write-up. |

The wire protocol is implemented three times:
* `app/.../Protocol.kt` (Android)
* `pc/companion/protocol.h` (PC)
* `pc/companion/web/protocol.js` (iPhone page)

All three are tested against `protocol/golden.txt`, so a change on one side alone fails a test. See
[protocol/PROTOCOL.md](protocol/PROTOCOL.md).

## Build

**Android:** `gradlew assembleDebug` (or open the project in Android Studio).

**PC** (Visual Studio 2022 Build Tools + Windows 11 SDK + CMake):
```
cd pc
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # add -DMYCAM_BUILD_TOOLS=ON for the dev tools
cmake --build build --config Release
```

**iPhone page:** there's no separate build. The files in `pc/companion/web/` are compiled into
`MyCamCompanion.exe` as resources, so rebuild the companion after editing them.

**Installer:** `powershell -ExecutionPolicy Bypass -File installer\build.ps1 -Version X.Y.Z` (needs Inno Setup 6).
The output is in `installer\output`.

## Tests

```
gradlew :app:testDebugUnitTest                 # Android: protocol encoding/parsing
ctest --test-dir pc/build -C Release           # PC: mycam_tests, plus web_golden (the iPhone page's protocol.js)
```

`web_golden` needs [Node.js](https://nodejs.org) and is skipped without it.

**iPhone server without an iPhone** (build with `-DMYCAM_BUILD_TOOLS=ON`):
```
pc\build\Release\web_probe.exe 120                               # runs the server alone, prints a pairing address
node pc/tools/webtest/fake_page.js "<pairing address>"           # routes, pairing, refusals, fake frames
node pc/tools/webtest/browser_check.js "<pairing address>"       # the real page in headless Chrome, real encoding
```

These register test devices. Delete them from `HKCU\Software\MyCam\WebDevices` afterwards.

## Install

1. **Android:** install the APK on the phone with
   `adb install app/build/outputs/apk/debug/app-debug.apk`, or copy it over. An iPhone needs nothing
   installed.
2. **PC:** run `MyCam-Setup-X.Y.Z.exe` (see [Build](#build)). It:
   * installs the [UsbDk](https://github.com/daynix/UsbDk) driver if needed (signed by Red Hat;
     hash-checked);
   * registers the MyCam camera;
   * adds the firewall rule for the iPhone page;
   * can start MyCam with Windows.

   Uninstall from Settings → Apps.
3. **Android over USB:** plug in the phone. The first time:
   * The phone asks to open MyCam. Tick **Always** and tap **OK**, then allow camera access.
   * Windows asks for admin once per new phone, to give its accessory mode the built-in WinUSB driver.
4. **iPhone:** follow [iPhone → Setup](#setup).

After that, plugging in (or opening the iPhone page) is all it takes.
* Left-click the tray icon for the settings window: camera, mirror, devices, start with Windows.
* Right-click it for the quick menu.

## Testing without a phone

```
"C:\Program Files\MyCam\MyCamCompanion.exe" --test-pattern [--rotate=90]
pc\build\Release\capture_test.exe out.bmp      # needs -DMYCAM_BUILD_TOOLS=ON
```

## Troubleshooting

The companion writes `%LOCALAPPDATA%\MyCam\mycam.log`, including the phone's own messages ("phone says: ...").
It records each step: phone found, switched to accessory mode, connected, camera started, frames decoded.
For the iPhone, look for the `web:` lines.

Exit MyCam from the tray rather than killing it. A killed companion can leave the phone stuck until it's
replugged.

**iPhone:**
* **The link doesn't open:** both devices must be on the same network, and Windows must call it
  **Private** (the firewall rule is private-only). Some guest and office Wi-Fi networks block devices
  from reaching each other.
* **"Couldn't start its iPhone page on port 47900":** another program is using that port. Check the log.
* **The page says the PC is busy:** another phone is using MyCam (a phone on the cable always wins).
* **The page asks to scan the code again:** the iPhone was forgotten on the PC, or Safari's website data
  was cleared.
* **The picture freezes when the iPhone locks:** this is expected. Safari stops the camera in the
  background, and the session waits 5 minutes for you to return.
* **The certificate warning came back:** the certificate was renewed (once a year), or the Windows user
  changed. Tap through it once more.

## Known limits

* Windows 11 only (it uses the `MFCreateVirtualCamera` API).
* **Android:**
  * The phone must support Android Open Accessory. Nearly all phones from the last 10+ years do.
  * While the companion is running, any Android phone you plug in switches to webcam (accessory) mode,
    so MTP file transfer isn't available. Exit MyCam from the tray to transfer files.
* **iPhone:**
  * It needs a browser with WebCodecs H.264 (current Safari).
  * It works over the network only; wired is still a plan.
  * The page must stay open in front: iOS stops the camera when Safari is in the background.

## License

MIT. See [LICENSE](LICENSE). Third-party parts keep their own licenses:
* [libusb](https://libusb.info) (LGPL-2.1, vendored in `pc/third_party/libusb`).
* [qrcodegen](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki (MIT, vendored in
  `pc/third_party/qrcodegen`).
* [UsbDk](https://github.com/daynix/UsbDk) (Apache-2.0, downloaded by the installer build).
* [DejaVu fonts](https://dejavu-fonts.github.io) (Bitstream Vera licence, with the DejaVu changes in the
  public domain).

The Windows XP look is an homage drawn from scratch: no Microsoft artwork or fonts are included.
