# MyCam: improvement backlog

Effort: **S** = small (hours), **M** = medium (a day or two), **L** = large (a week or more).
Status: ✅ done · 🟡 partly done · ⏳ to do. Day-to-day tracking lives in [TASKS.md](TASKS.md).

## Field test results

| Scenario | Result | Notes |
|---|---|---|
| Long video call | ✅ Uninterrupted | 2026-10-09 |
| Phone screen locked while streaming | ✅ Uninterrupted | 2026-10-09 |
| Unplug and replug | ✅ Reconnects instantly | 2026-10-09 |
| Opening the phone's own camera app | ✅ Stream stops | Correct behavior. **Do not change.** |
| Pause / resume (phone and PC) | ✅ Works | 1.1.0, user test 2026-10-09 |
| Windows locked while streaming | ✅ Pauses, resumes on unlock | Fixed in 1.1.0 (was a privacy issue); user test 2026-10-09 |
| Install, upgrade, uninstall (installer) | ✅ Works | Tested on this PC, 2026-10-09 |
| **Clean install** from the GitHub release (no MyCam, no UsbDk, no phone driver binding) | ✅ Works | 1.2.0, 2026-10-09: installer brought UsbDk, first-use phone switch + WinUSB prompt worked, streaming 1080p, 0 decode errors |
| Signed release APK (R8-shrunk, 1.5 MB) | ✅ Works | 1.2.0 on the phone, back and front camera |

The shipped binaries depend only on DLLs that are part of Windows (checked with `dumpbin`): no Visual C++
runtime, nothing from the build tools. Windows 11 **N** editions need the Media Feature Pack (video codecs).
A test on a *different* PC is still worth doing; Windows Sandbox can't run here because CPU virtualization
is off in this PC's firmware.

Still untested: pause from the phone's **notification**, pause **surviving unplug/replug**, PC sleep and
resume, killing or crashing the companion mid-stream, and two phones plugged in at once.

---

## 1. Privacy and control: ✅ done (1.1.0)

### 1.1 Pause button on the phone **and** the PC (S): ✅
**Pause / Resume** in four places: the phone app, the phone's notification, the PC tray menu and the PC
settings window. Pausing on either side pauses both: the phone camera turns fully off, and apps see an
Aero **"Camera paused"** picture instead of black or a frozen frame.
- The phone owns the pause state and remembers it across reconnects, so a cable glitch never turns a
  paused camera back on.
- Protocol v2: PC → phone `CMD_PAUSE` / `CMD_RESUME`; phone → PC `STATE_PAUSED`.
- When there's no phone video (not connected, starting, phone problem), the camera shows a
  "Waiting for the phone" picture instead of black.

### 1.2 Pause when Windows is locked or asleep (S): ✅
The phone camera turns off on lock or sleep and comes back on unlock. This is separate from the manual
pause: unlocking never undoes a pause you chose. As a guard, no live video reaches the camera while paused
or locked, even frames already in flight.

### 1.3 Aero design on the phone app (M): ✅
Glass header, Win7 command link for Pause / Resume, Aero buttons, blue "main instruction" status,
LIVE pill, Selawik font, new launcher icon (adaptive, legacy and themed).

---

## 2. Reliability

### 2.1 Recover from a force-kill or crash (S): 🟡
- ✅ A phone left streaming after the companion died is now told to stop as soon as a new companion
  connects.
- ✅ Streaming now goes over WinUSB, so a killed companion normally no longer leaves the phone stuck behind
  UsbDk.
- ⏳ The UsbDk fallback path (first connection, before the WinUSB driver is bound) can still leave the
  phone hidden until replugged. Detect it and reset the port, or tell the user to replug.

### 2.2 Fewer UAC prompts on first use (S): 🟡 built in 1.3.2, needs a new-phone test
A new phone used to prompt twice: once at install, then again for WinUSB binding.
- ✅ The installer registers the scheduled task "MyCam phone driver" (runs as SYSTEM, only
  `MyCamCompanion.exe --bind-driver`). Signed-in users may start it but not change it (verified: a non-admin
  start works; a non-admin re-register gets "Access is denied"). The uninstaller deletes it.
- ✅ The companion starts the task instead of asking for UAC, and only trusts it if it still points at its
  own exe. If the task is missing, it falls back to the old one-time UAC prompt.
- ⏳ End-to-end check: a phone that has never been connected should go straight to "Ready" with no prompt.
  The test phone is already bound. Use another phone, or remove the accessory device in Device Manager
  (View > Show hidden devices) and replug.

### 2.3 Test the remaining scenarios (S–M): ⏳
See "Still untested" above, plus phone low-battery mode and Android killing the service under memory
pressure. Fix whatever breaks.

---

## 3. Picture quality and camera control: ✅ done (1.3.1), except phone-side latency

### 3.1 Crop to fill (S): ✅ (1.3)
A portrait phone gives a narrow image with black bars. Add a "Crop to fill" option that center-crops to
16:9 (`DrawFittedNV12` in `pc/vcam/frame_transform.cpp`), with a toggle in the settings window.

### 3.2 Resolution and frame-rate options (M): ✅ (1.3); 30 / 60 / 120 fps via high-speed capture in 1.3.2 (needs a test)
Correction: the test phone's back camera *does* offer a constrained high-speed mode (1080p and 720p at 120 and
240 fps); only the normal modes stop at 30. 1.3.2 opens a high-speed session at 120 fps and the encoder keeps
every other frame (`KEY_MAX_FPS_TO_ENCODER`, Android 10+), giving 1080p60. Short exposure (≤ 8 ms) makes it
noisier in dim light. If the phone refuses the session, the stream falls back to 30 fps. The front camera has
no high-speed mode, so it offers only 30 fps.
1.3.2 also offers **120 fps** (the high-speed capture passed straight through, 30 Mbps) as a third choice on
both apps; each choice is enabled only if the camera and encoder can do it (CAMERA flag 0x40), and the
virtual camera offers 1080p120 and 720p120 to apps. Noise is the same at 60 and 120 (same exposure limit).
Availability is worked out per quality (CAMERA frame-rate masks, 21-byte payload), by the same
`modeFor()` check that starts the camera, so a phone that only does 4K30 never offers 60/120 at 4K.
720p / 1080p / 4K, and 60 fps where the phone supports it. Add a protocol command to request a mode, and
advertise the matching media types from the virtual camera.

### 3.3 Camera controls (M): ✅ (1.3), plus an Auto reset button (1.3.1). Lens choice via zoom ratio only where the phone exposes it (the test phone: 1–10×, no ultrawide).
Zoom, tap-to-focus, exposure, torch, and lens choice (ultrawide / telephoto). Phone UI first; exposing them
to PC apps through `IKsControl` is a stretch goal.

### 3.4 Lower latency (M): 🟡 PC side done (1.3–1.3.1): delivery on arrival, one less copy, adaptive bitrate when the USB link is the limit. Measured: PC decode+copy 3–12 ms; **phone capture→encoded 150–230 ms** (MediaTek encoder buffers ~3 frames). Next: try encoder low-latency vendor keys, fewer camera buffers, I-frame interval.
Decode on the GPU (D3D11 / DXVA) instead of the CPU, cut extra frame copies, and add a latency
measurement (timestamp in the frame header → log).

Phone-side round (built, needs a test on the phone):
- The log splits the delay: `latency: ... (camera X ms, encoder ~Y ms)` (sensor → capture result vs.
  sensor → encoded frame), so we know which half to attack.
- Camera: electronic stabilisation off (it holds frames back to look ahead), fast noise reduction and
  edge modes, where offered.
- Encoder: no B-frames; on Android 12+ every vendor parameter with "low-latency" in its name is switched
  on, and the full list is logged (`encoder vendor parameters: ...`) for further tuning.
- PC: 16 queued USB reads (256 KB) instead of 4, so a 4K frame no longer stalls the phone's write while
  the previous frame decodes.

---

## 4. Phone app

### 4.1 Dim the screen while streaming (S): 🟡 built, needs a test on the phone
- After 30 s untouched while streaming (or the new "Dim the screen" command link) the app shows a nearly
  black screen at 2 % brightness; it drifts every minute against OLED burn-in. Any touch wakes it (that
  touch does nothing else).
- Heat: the service listens to `PowerManager` thermal status (Android 10+), warns on both screens from
  MODERATE up, and copies changes to the PC log.
Original plan: a dark aurora "streaming" screen keeps the phone cooler, saves battery and is more private,
with a warning when the phone gets hot.

### 4.2 Branding and release signing (S): ✅ (1.2.0)
- ✅ Real app icon (Aero webcam, adaptive + legacy + themed).
- ✅ Package name `io.github.whoissaaif.mycam` (was `com.example.mycam`). Android treats it as a new app:
  uninstall the old one once.
- ✅ Release signing: `tools\create-release-key.ps1` makes the key in `%USERPROFILE%\.mycam\` and a git-ignored
  `keystore.properties`; `gradlew assembleRelease` then signs. **Back up the key and its password.**

---

## 5. PC app: ✅ done (1.0–1.1), except code signing

### 5.1 Tray icon that shows status (S): ✅
Aero webcam with a status badge: disconnected (grayscale), ready, streaming (green), paused (amber),
error (red). 16–256 px, DPI-aware.

### 5.2 Settings window (M): ✅
Aero window (Direct2D): glass title bar, live status with a LIVE pill, Pause / Resume, back/front camera,
mirror, start with Windows, reconnect, open log folder. Keyboard: Tab / arrows, Space / Enter, Esc.
- ⏳ Screen readers: a UI Automation provider (`uia_provider.cpp`, redesign.md §8.4) gives one fragment per
  element of `ui_model.h` (Invoke, Toggle, Selection / SelectionItem per radio set, ExpandCollapse, read-only
  RangeValue) and makes the status headline and sentence live regions. Checked with `pc/tools/uia_dump`
  (`-DMYCAM_BUILD_TOOLS=ON`); still to try with Narrator itself.

### 5.3 Real installer (M): ✅, code signing pending
Inno Setup wizard with Aero artwork (`installer/`, built by `installer/build.ps1`). It installs UsbDk if
needed, registers the camera, offers "Start with Windows", and lists MyCam in Settings → Apps. Upgrades
close the running companion cleanly; the uninstaller asks before removing UsbDk.
- ⏳ **Code signing.** Needs a code-signing certificate; `mycam.iss` is ready for it (`/DSIGN`).
- ⏳ **Remove UsbDk completely.** UsbDk's own uninstaller (`msiexec /x`) unhooks it from USB but leaves its
  service and `UsbDk.sys` behind, even after a restart (inert, but not clean). When the user chooses
  "Also remove UsbDk", the uninstaller should also run `sc delete UsbDk` and delete
  `%WINDIR%\System32\drivers\UsbDk.sys` (found in the clean-install test, 2026-10-09).
- Licensing: libusb is LGPL-2.1 and statically linked, so anyone given the binaries must also be able to
  get the source. The GitHub repo is private: if you share builds, share the source too (or make the repo
  public).

---

## 6. Project hygiene: ✅ done
- ✅ Git, with dev tools kept out of shipped builds (`-DMYCAM_BUILD_TOOLS=ON`).
- ✅ Unit tests: C++ (`pc/tests`: packet parser, frame transform, BGRA→NV12) and Android (`ProtocolTest`).
- ✅ Protocol spec in `protocol/PROTOCOL.md` (v2); both sides tested against `protocol/golden.txt`.

---

## 7. Design language and assets: 🟡 Windows XP redesign built (branch `xp-redesign`), testing left

### 7.1 Decision: ✅ Windows XP (Luna Blue), replacing Windows 7 Aero (2026-10-10)
The owner moved the look from Win7 Aero (the 2026-10-09 decision) to **Windows XP, Luna Blue only**:
- no dark mode;
- a new friendly camera logo, because the old webcam read as an eye and felt like surveillance;
- a camera preview on the PC only;
- a sunny "hills and sky" pause picture. The photo in the local `inspo/` folder is a reference only; the
  scene is painted by script.

The full spec, findings and roadmap are in [redesign.md](redesign.md).

### 7.2 Style guide (S): ✅ [design/STYLE.md](design/STYLE.md)
The tokens live in `app/.../ui/theme/Color.kt`, `pc/companion/xp_draw.cpp` and `design/tools/luna_draw.ps1`.
Type: DejaVu Sans on the phone (shipped, free licence). Tahoma and Trebuchet MS on the PC (installed with
Windows, not shipped).

### 7.3 Assets: ✅ redrawn for XP
All of it is drawn from scratch by scripts in `design/tools/` (shared library `luna_draw.ps1`).

| Asset | Status |
|---|---|
| "Snap" logo: Windows `.ico`, Android adaptive / legacy / round / themed, notification icon | ✅ |
| Status family (tray, window, phone): ready, streaming (green ▶), paused (amber ‖), error (red ×), disconnected (grey) | ✅ |
| Camera pictures: "Camera paused" (hills + frosted glass card), "Waiting for the phone" (morning tint + animated green marquee drawn by the companion) | ✅ |
| Installer art, Play Store icon and feature graphic | ✅ |
| Phone first-run and Dim-screen hills (drawn in Compose) | ✅ |

### 7.4 What the redesign changed (S–L): 🟡 built, testing left
- **Phone:**
  - Now / Settings tabs and XP task groups, with the hero Pause.
  - A pairing dialog with a blurred backdrop and a countdown bar, and per-PC Forget.
  - First-run cards, a landscape two-pane layout, per-state frame-rate reasons, and the night-hills Dim screen.
  - Accessibility semantics.
- **PC:**
  - A 720 × 500 task-pane window that fits small screens, with a live preview (frosted LIVE / mode chips).
  - A pairing dialog with Cancel and a countdown, a green marquee while connecting, keyboard groups and
    access keys, and tray check marks plus a Quality submenu.
  - A UI Automation provider.
- **Left to do:** real-phone tests of every flow (USB and Wi-Fi), a Narrator pass, and checking high contrast
  and reduced motion by eye.

### Notes and risks
- **Tray menu:** uses the native Windows 11 menu, which is accessible.
- **Accessibility:** every text pair meets WCAG AA. Amber and red are used only in badges and icons.
- **Licensing:** no Microsoft artwork, wallpapers or fonts are shipped.

---

## 8. Publishing and releases: 🟡

- ✅ GitHub repo (private): https://github.com/whoissaaif/mycam (MIT license).
- ✅ Commit history uses a GitHub no-reply email; `inspo/` and raw screenshots are excluded.
- ✅ Releases **v1.1.1** and **v1.2.0** (installer + APK; 1.2.0 is signed with the release key).
- ✅ Release checklist in [TASKS.md](TASKS.md#releases).
- ⏳ Decide public vs. private before sharing (download links and the phone's "get the app" link only
  work for you while the repo is private).

---

## 9. iPhone support (L): 🟡 web camera built (branch `aiyan-webtest`), iPhone tests left

Aiyan's part of the project ([docs/AIYAN-IPHONE.md](docs/AIYAN-IPHONE.md)).

**Approach: a web page, not an app.** A native iOS app needs a Mac with Xcode and the $99/year developer
program, so the iPhone uses Safari instead ([docs/iphone-web-plan.md](docs/iphone-web-plan.md)). The PC
companion serves a page over HTTPS. The page:
- captures the camera;
- encodes H.264 on the phone's hardware (WebCodecs);
- sends normal MyCam packets over a secure WebSocket, one per message
  ([docs/web-camera-format.md](docs/web-camera-format.md)).

On the PC, the page is a third transport in the one single-threaded `SessionLoop`, next to USB and Android
Wi-Fi. Everything after the transport carries over unchanged: decoding, the virtual camera, pause and
lock-pause, the status pictures, the controls. It also works on iPads, on Android phones without the
app, and in desktop browsers.

### 9.1 Wireless (the web page): 🟡 built, iPhone tests left
- [x] PC: HTTPS + WebSocket server in the companion (`web_server.cpp`):
  - Schannel TLS with a self-signed certificate made once per user (`tls_server.cpp`);
  - the page embedded in the exe;
  - port 47900.
- [x] PC: the page as a transport in `SessionLoop` (`phone_link_web.cpp`), with no new threads.
  - A plugged-in phone takes over.
  - Other pages wait while a phone is connected.
  - A paused page that iOS disconnects keeps its session for 5 minutes.
- [x] Pairing: QR code → one-time code (128 bits, 5 minutes) → `POST /pair` → device token (256 bits;
  the PC stores only its hash). `/ws` refuses everything else.
- [x] Settings: "Connect an iPhone" page with the QR code (Nayuki's qrcodegen, MIT), a choice of network,
  and remembered iPhones with Forget. An iPhone card on Home.
- [x] Installer: inbound firewall rule, TCP 47900, private networks only; removed on uninstall.
- [x] Page (XP Luna look):
  - Start, pause/resume, back/front camera, dim screen, wake lock;
  - CAMERA frame-rate masks from the camera *and* the encoder;
  - back-pressure with bitrate steps;
  - STATE(paused) when iOS takes the camera, and reconnects by itself.
- [x] Tests:
  - WebSocket/HTTP/handshake/tokens in `mycam_tests`;
  - `protocol.js` against golden.txt (`web_golden`, needs Node.js);
  - end to end with `web_probe`, `fake_page.js`, and `browser_check.js` (the real page in headless Chrome
    with a fake camera).
- [ ] On the iPhone 13: the definition of done in AIYAN-IPHONE.md §5. This covers rotation, lock and
  resume, 1080p30 for 10 minutes, 1080p60, the measured delay, and Forget.

### 9.2 Wired: 🟡 foundation built, test left ([docs/iphone-wired.md](docs/iphone-wired.md))
- [x] The PC recognises an iPhone's USB hotspot adapter, offers it in the network choice and reports
  "USB hotspot".
- [ ] Test Personal Hotspot over USB (Apple Devices app), and fill in the findings table.
- [ ] Native app + usbmux: only if a Mac and the developer program become available. The old code on
  branch `aiyan` is reference only (two threads, can deadlock).

**Differences from Android (to tell users)**

| | Android | iPhone (web page) |
|---|---|---|
| Starting | The app opens itself when plugged in | Open the MyCam page (or its Home Screen icon) and tap Start |
| Phone locked or Safari in the background | Keeps streaming | Pauses: apps see "Camera paused"; it resumes when the page is back in front |
| Brightness and focus controls | Yes | No (Safari can't set them); zoom and torch where the camera has them |
| PC setup | MyCam installer | MyCam installer; tap through Safari's certificate warning once |

Continuity Camera (Apple's built-in iPhone webcam) only works with Macs, so it doesn't help on Windows.

---

## 10. Windows 10 support (M): ⏳

Today the installer requires Windows 11, because the PC shows "MyCam" through `MFCreateVirtualCamera`,
which only exists on Windows 11 (build 22000+). Everything else (USB, WinUSB binding, decoding, tray,
settings window, installer) already works on Windows 10.

**Plan: a DirectShow camera for Windows 10 (the approach OBS Virtual Camera uses)**
- A second small COM DLL registered as a DirectShow video capture source. It reads the same shared memory
  (`Global\MyCamFrame`), so the phone app, protocol and USB code don't change.
- The installer picks by Windows version: Windows 11 keeps the current virtual camera, Windows 10 gets
  the DirectShow one. `MinVersion` drops to Windows 10 (1809 or newer).
- No driver and no driver signing.

**What works on Windows 10 with it**

| Works | Doesn't work |
|---|---|
| Zoom, Discord, Skype, OBS, most desktop video-call apps | The built-in Windows Camera app |
| Chrome, Edge, Firefox (Google Meet, web Teams, etc.) | Some newer Store (UWP) apps that only see real cameras |

**Rejected alternative:** a kernel camera driver (AVStream). It would appear in every app, but needs
kernel code plus a Microsoft-approved signature (paid EV certificate, attestation process). Too much
cost and risk for this project.

**Work items**
- [ ] DirectShow source filter (formats 1080p/720p/4K, 30/60/120; status pictures; pause), reusing
      `frame_reader` and `frame_transform`
- [ ] Installer: register the right camera per Windows version, lower `MinVersion`, uninstall both
- [ ] Settings window: "camera registered" check for the DirectShow camera
- [ ] Test on a real Windows 10 PC (this dev PC can't run Sandbox or VMs): Zoom, Chrome/Meet, OBS, Discord

**Needs before starting:** a Windows 10 PC for testing.

---

## 11. Wireless option (L): 🟡 phases 1 and 2 built on branch `wireless` (1.4.0-beta3); a few tests left before merging

MyCam is wired-only by design (plug and play, no setup, no lag spikes, phone charges while streaming).
A wireless mode would be an **optional** second way to connect, with the cable staying the default.

**How it would work**
- Phone and PC on the same Wi-Fi (or the PC on the phone's hotspot). The phone app gets a "Wireless" switch
  that starts the same streaming service over a TCP connection instead of the USB accessory.
- **Discovery:** the PC companion finds phones on the local network (mDNS / DNS-SD, e.g.
  `_mycam._tcp`), so nobody types IP addresses.
- **Pairing, once per phone:** the PC shows a 6-digit code (or a QR code) and the phone confirms it.
  Unpaired devices are refused. A webcam on the network must not be open to anyone on the same Wi-Fi.
- **Encryption:** TLS with the key from pairing, so the video can't be watched on the network.
- **Same protocol** (`protocol/PROTOCOL.md`) on top of TCP. The PC side becomes a second transport next to
  USB. Aiyan's `ITransport` refactor (branch `aiyan`) is the natural base once its threading issues are fixed.
- **Adaptive bitrate** already exists and matters more here: Wi-Fi throughput and delay vary a lot.

**Differences from wired (to explain to users)**

| | Wired (USB) | Wireless (Wi-Fi) |
|---|---|---|
| Setup | Plug in | Same network + one-time pairing code |
| Delay | Lowest | Higher, with occasional spikes on busy Wi-Fi |
| Quality | Up to 4K / 120 fps | 1080p30 recommended; 4K and 120 fps only on strong 5 GHz Wi-Fi |
| Battery | Phone charges | Phone drains: warn in the app, keep the dim screen (4.1) |
| Starting | App opens itself when plugged in | Open MyCam on the phone and turn on Wireless |
| Windows | Nothing extra | Nothing extra: the PC only connects out, so there is no firewall prompt |

**Phase 1 (tested 2026-10-10, CMF Phone 1 / Android 16):** found in ~1 s, 1080p30 live through the virtual camera, phone-side delay ~106 ms, PC decode ~8 ms. The "Waiting for the phone" picture now mentions Wi-Fi.

Design: UDP discovery + TCP on the phone ("Use over Wi-Fi" switch) and PC
("Find phones on Wi-Fi"); the phone asks "Allow <PC>?" for every new connection (in the app and as a
notification); the cable always wins; Wi-Fi lock and a small send buffer on the phone keep delay low; the PC
side shares one single-threaded session loop with USB (no new threads). No pairing or encryption yet.

**Phase 2 (built and tested 2026-10-10):** one-time pairing with a 6-digit code compared on both screens (with a commitment, as in Bluetooth numeric comparison), then P-256 ECDH + HKDF + AES-256-GCM for every session; paired PCs connect without asking. Details in PROTOCOL.md "Wireless security". Tested live: pairing (codes matched), encrypted 1080p stream (0 decode errors), paired reconnect in 40 ms with no prompt, PC forgot the phone -> paired again with a new code, "Don't allow" -> no retry for 2 minutes. Interop proven with JDK-generated vectors in both test suites. Still to test before merging: cable takeover, 60/120 fps and 4K over Wi-Fi, a busy network.

**Work items**
- [ ] Phone: TCP server/client mode in the streaming service, mDNS advertising, pairing screen, battery warning
- [ ] PC: network transport, discovery list in the settings window, pairing UI, firewall rule in the installer
- [ ] Security: pairing, TLS, and refusing unpaired devices; review before release
- [ ] Bitrate/latency tuning for Wi-Fi; default to 1080p30 when wireless
- [ ] Testing: 2.4 vs 5 GHz, hotspot, Wi-Fi drop and reconnect, switching between cable and Wi-Fi

## 12. QR pairing for Android (M): 🟡 built (2026-10-13), device tests left

Pairing over Wi-Fi today means turning on Wi-Fi mode, finding the PC, waiting for a 6-digit code,
comparing it on two screens and tapping Allow. It also depends on UDP broadcasts, which many routers drop.
The PC already shows a QR code for iPhones, so Android phones should scan **the same code**: scan once, and
the phone is paired and connected. The wire format is in PROTOCOL.md "QR pairing". This section covers the
design and the work.

**Decisions (2026-10-12, owner):**
- **One QR code for both kinds of phone:** Safari for iPhones, MyCam's own scanner for Android.
- **Scanner:** ZXing `core`, bundled (about 0.5 MB), on our own Camera2 code. It doesn't need Google Play
  services, so it works on every phone.
- **The 6-digit pairing stays as the fallback** ("Can't scan?" → Find devices). From now on it needs Allow
  on the PC as well as on the phone. Before, a device on the network posing as a phone could approve its
  own pairing in milliseconds and then send video into the MyCam camera.
- **Paired phones still reconnect through discovery by themselves.** `/app/connect` also lets the phone
  reconnect directly.

**Why the key exchange is safe without a code comparison:**
- The QR code carries the PC's certificate fingerprint, so the phone's HTTPS connection is pinned to that PC.
- The one-time code proves the phone saw the PC's screen in the last 5 minutes.
- `K` then travels inside that pinned TLS connection.
- From then on it is the ordinary paired handshake, with no new cryptography in the session itself.

**Phone flow:**
1. On the Connect page, **Scan QR Code** opens a full-screen scanner (portrait while it shows). It asks for camera permission if needed. While a stream is live the row is disabled
   with "Stop the stream to scan" (the camera is in use).
2. The scanner finds a code with `t` and `f` and gives a short vibration. It closes straight away and
   shows "Connecting to <PC>…" for the second the next step takes. **No code is shown and nothing asks for
   confirmation, on the phone or the PC (owner, 2026-10-13):** scanning is the consent, and it goes straight
   to connected.
3. The phone turns wireless mode on if it's off, calls `/app/pair`, stores `K`, then calls
   `/app/connect`. The PC connects within about a second and the stream page shows the PC as connected.
   Scanning a PC the phone is already paired with simply pairs again (a new `K` replaces the old one on
   both sides), so there is one path and nothing to go stale.
4. Errors, in plain words, with **Scan again** and **Close**:
   - Wrong certificate: "This isn't the PC in the QR code".
   - Code used or expired: "Scan the new code the PC shows".
   - The PC can't be reached: "Same Wi-Fi? Is the PC's network set to Private?".
   - Paired, but the PC is busy with another phone, or never connected within 10 s.
   - Code without `f` (scanner screen): "Update MyCam on the PC".
   - Not a MyCam code: say nothing and keep scanning.
5. The phone remembers the PC's last address with its pairing, for a later direct `/app/connect`.

**PC flow:**
- On Home, the QR card is called **"Generate QR"** (done 2026-10-13; its button is "Show QR code").
- The "Connect an iPhone" page is now **"Connect a phone"**, with the same QR code and steps for each kind
  of phone. A QR pairing needs no balloon of its own: the usual "Phone connected over Wi-Fi" one follows.
- The 6-digit pairing dialog and the window's pairing page have **Allow** and **Don't allow** ("Don't allow"
  has the keyboard focus first, so a stray Enter never allows). The window's Cancel button had never been
  wired up (`model.cancelPairing` was unset); it works now.

**Work items:**
- [x] PC `tls_server`: the certificate's SHA-256 fingerprint (`Credentials::Fingerprint`).
- [x] PC `web_server` / `web_proto`: `f` in the QR URL, `/app/pair` and `/app/connect`; `FromHex`,
      `FromBase64Url` and `CertFingerprint` are pure and unit-tested.
- [x] PC `PhoneLink`: `AppPair` / `AppConnect` and a direct connect (`direct_`, run from the idle loop).
- [x] PC: Allow / Don't allow on code pairing; a phone refused here is never auto-connected again
      (`WifiEnd::RefusedHere`) until the companion restarts.
- [x] PC UI: "Generate QR" on Home, "Connect a phone" page, pairing dialog and page buttons.
- [x] Phone: the scanner (`ui/QrScanner.kt`: Camera2 → `ImageReader` YUV → ZXing on the camera thread).
      No torch: a QR code on a screen glows by itself, and a torch only adds glare.
- [x] Phone: pinned HTTPS (`QrPairing.kt`), `/app/pair`, `/app/connect`, the PC's address remembered
      (`PairedPcs.setAddress`), status and errors on the Connect page.
- [x] Golden vectors `qr.*` (JDK), checked by `QrPairingTest` and `mycam_tests`.
- [ ] Phone: use the remembered address to reconnect directly (a "Connect" on a paired PC in Settings).
- [ ] Page (`app.js`): on Android, offer "Open in the MyCam app" (an `intent:` link plus a `mycam://` intent
      filter in the app) or "Use this page instead". The page already drops `f` with the rest of the query.
- [~] Tests on devices. **Passed 2026-10-13** (CMF Phone 1, release APK, PC on Ethernet):
      - Scan to connected in about 1.5 s; the PC logs QR pair, connect request and encrypted link within
        100 ms.
      - 1080p30 over the QR-paired link into the MyCam camera (ffmpeg: 241 frames in 8 s).
      - PC network on Public: "Can't reach the PC… set to Private", and nothing reaches the PC.
      - Scanning again while connected: re-pairs quietly. Two bugs found and fixed: the phone said "busy
        with another phone", and the card would have stuck on "Connecting…".
      - 6-digit fallback: the PC had forgotten the phone, so it paired again with a code. "Don't allow" on
        the PC means no key stored and the phone's link drops. Allow on both stores the key and connects.
      - Swiping MyCam from recent apps stops the service, the link and the notification.

      **Still to test:**
      - A second Android phone.
      - An iPhone scanning the same code.
      - An expired code.
- [x] Release APK (R8) on a phone: ZXing and the pinned TLS client work under minification.
- Known, minor:
  - In a code pairing where the phone allows first, the phone says "Wi-Fi: connected" while the PC is still
    deciding, and keeps a key if the PC then refuses. That key is cleaned up by the forgot-me path next time.

---

## Suggested order

1. **2.3** Test the untested scenarios (cheap, and it finds the real bugs)
2. **2.1 / 2.2** Remaining reliability and first-use polish
3. **3.1** Crop to fill, and **4.1** dim the screen while streaming
4. **7.2** Write `design/STYLE.md`
5. Everything else as needed (3.2–3.4, 5.2 accessibility, 5.3 code signing)
