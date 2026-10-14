# Architecture

The technical deep dive: how the pieces fit together, the repo layout, and how to build and test from
source. For the user-facing pitch, see the [README](../README.md). For the day-to-day build/test commands
and the project's own rules, see [CLAUDE.md](../CLAUDE.md).

## How it works (Android, USB)

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
* **Wi-Fi (Android)** carries the same packets over an encrypted TCP link after a one-time pairing. See
  "Wireless transport" in [protocol/PROTOCOL.md](../protocol/PROTOCOL.md).
* **One phone at a time.** A phone on the cable always wins over Wi-Fi and the iPhone.

## iPhone

An iPhone has no app. The companion runs a small HTTPS server, and Safari opens the MyCam page from it.
The page encodes H.264 on the iPhone's hardware (WebCodecs) and sends the **same MyCam packets** as
Android, one per WebSocket message. From the decoder onward, the PC treats it exactly like an Android
phone.

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

**Pairing (first time):**

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

Wired iPhone isn't supported yet. The groundwork and the plan (using the iPhone's USB Personal Hotspot)
are in [iphone-wired.md](iphone-wired.md). The full server/route/token reference is in
[web-camera-format.md](web-camera-format.md), and the assignment is in [AIYAN-IPHONE.md](AIYAN-IPHONE.md).

## Repo layout

| Path | What |
|---|---|
| `app/` | Android app (Kotlin, Compose). `WebcamService` owns the link (USB or Wi-Fi) and the camera. |
| `pc/companion/` | Tray app: the AOA switch (libusb + UsbDk), streaming over WinUSB, Wi-Fi, the iPhone server, H.264 decode, the frame hand-off. |
| `pc/companion/web/` | The iPhone page (`index.html`, `app.js`, `protocol.js`, `style.css`), embedded in the exe through `companion.rc`. |
| `pc/companion/web_server.cpp`, `tls_server.cpp`, `web_proto.cpp`, `phone_link_web.cpp` | The iPhone server: HTTPS + WebSocket, the Schannel TLS and certificate, the pure parsing and token code (unit-tested), and the transport in `SessionLoop`. |
| `pc/vcam/` | Virtual camera media source DLL loaded by Windows Frame Server. |
| `pc/common/` | Shared memory layout and the COM CLSID. |
| `pc/third_party/` | Vendored libusb (trimmed to the Windows backend), and [qrcodegen](https://www.nayuki.io/page/qr-code-generator-library) for the iPhone QR code. |
| `installer/` | Inno Setup installer (`mycam.iss`) and its build script (`build.ps1`). |
| `pc/install.ps1` / `uninstall.ps1` | Developer install/removal without building the installer. |
| `design/` | Art tools (`tools/*.ps1`) and previews. Style: Windows XP Luna Blue ([design/STYLE.md](../design/STYLE.md)). |
| `pc/tests/` | C++ unit tests: packet parser, protocol vectors, frame rotate/scale, WebSocket frames, tokens. |
| `pc/tools/` | Developer tools (not shipped): `capture_test`, `usb_probe`, `web_probe`, and the iPhone page checks in `webtest/`. |
| `protocol/` | Wire protocol spec (`PROTOCOL.md`) and byte-exact test vectors (`golden.txt`). |
| `docs/` | Design notes and this architecture reference. |

## The wire protocol

Implemented three times:
* `app/.../Protocol.kt` (Android)
* `pc/companion/protocol.h` (PC)
* `pc/companion/web/protocol.js` (iPhone page)

All three are tested against `protocol/golden.txt`, so a change on one side alone fails a test. See
[protocol/PROTOCOL.md](../protocol/PROTOCOL.md).

## Build and test from source

**Android:** `gradlew assembleDebug` (or open the project in Android Studio).

**PC** (Visual Studio 2022 Build Tools + Windows 11 SDK + CMake):
```
cmake -S pc -B pc/build -A x64         # add -DMYCAM_BUILD_TOOLS=ON for the dev tools
cmake --build pc/build --config Release
```

**Installer:** `powershell -ExecutionPolicy Bypass -File installer\build.ps1 -Version X.Y.Z` (needs
[Inno Setup 6](https://jrsoftware.org/isinfo.php)). The output is in `installer\output`.

**The iPhone page** has no separate build: the files in `pc/companion/web/` are compiled into
`MyCamCompanion.exe` as resources, so rebuild the companion after editing them.

### Tests

```
gradlew testDebugUnitTest                      # Android: protocol encoding/parsing
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

**Testing without a phone:**
```
"C:\Program Files\MyCam\MyCamCompanion.exe" --test-pattern [--rotate=90]
pc\build\Release\capture_test.exe out.bmp      # needs -DMYCAM_BUILD_TOOLS=ON
```

Both suites also run in CI on every pull request (`.github/workflows/ci.yml`).
