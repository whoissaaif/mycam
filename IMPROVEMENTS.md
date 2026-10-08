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

### 1.1 Pause button on the phone **and** the PC (S)
**Pause / Resume** controls in four places: the phone app, the phone's notification, the PC tray menu and
the PC settings window. Pausing on either side pauses both: the phone camera turns fully off and the PC
shows "Paused" everywhere. The person on the other end of a call sees an Aero-style **"Camera paused"**
picture instead of black or a frozen frame, so they know it's intentional.
- The phone owns the pause state, so both sides always agree. It's remembered across reconnects: a cable
  glitch never turns a paused camera back on.
- Protocol v2: PC → phone `CMD_PAUSE` / `CMD_RESUME`; phone → PC `STATE_PAUSED`. The phone ignores
  `CMD_START` while paused.
- When no phone video is available (not connected, starting, phone problem), the PC camera shows a
  "Waiting for your phone" picture instead of black.

### 1.2 Pause when Windows is locked (S): privacy fix
Turn the phone camera off when the Windows session is locked or the PC goes to sleep, and resume on
unlock. This is separate from the manual pause: unlocking never undoes a pause you chose yourself.
- Companion: `WTSRegisterSessionNotification` (`WTS_SESSION_LOCK` / `UNLOCK`) and `WM_POWERBROADCAST`
  (suspend / resume).
- While locked, the PC camera shows the "Camera paused" picture, and the tray reads "Paused while Windows
  is locked".

### 1.3 Aero design on the phone app (M)
Bring the Android app into the design language (section 7) while its screens are being changed anyway:
glass header, Aero buttons, Win7 "main instruction" status, LIVE pill, Selawik font, and the new webcam
launcher icon.

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

## 5. PC app: ✅ done (2026-10-09), except code signing

### 5.1 Tray icon that shows status (S): ✅
Aero webcam icon with a status badge: disconnected (grayscale), ready, streaming (green), paused (amber,
for 1.1), error (red). 16–256 px, DPI-aware via `LoadIconMetric`. Drawn by `design/tools/make_icons.ps1`.

### 5.2 Settings window (M): ✅
Aero-style window (Direct2D): glass title bar, live status with a LIVE pill, back/front camera, mirror,
start with Windows, reconnect and open the log folder. Left-click the tray icon to open it; it's also the
default item in the tray menu. Keyboard: Tab / arrows, Space / Enter, Esc. Resolution and crop options
arrive with 3.1 / 3.2.
- Follow-up: the controls are custom-drawn, so screen readers don't see them yet (needs UI Automation
  support).

### 5.3 Real installer (M): ✅, code signing pending
Inno Setup wizard with Aero artwork (`installer/`, built by `installer/build.ps1`). It installs UsbDk if
needed, registers the camera, offers "Start with Windows", and lists MyCam in Settings → Apps. Upgrades
close the running companion cleanly. The uninstaller removes everything, and asks before removing UsbDk.
- Still to do: **code signing**. This needs a code-signing certificate; `mycam.iss` is ready for it (`/DSIGN`).
- Licensing before public distribution: libusb is LGPL-2.1 and statically linked. Ship the app's source,
  or link libusb dynamically, so users can relink it. The UsbDk (Apache-2.0) and libusb licenses are
  already installed under `licenses\`.

---

## 6. Project hygiene (S): ✅ done
- ✅ Git repo set up and committed.
- ✅ `capture_test` and `usb_probe` are behind `-DMYCAM_BUILD_TOOLS=ON` and never shipped.
- ✅ Unit tests: C++ (`pc/tests`, packet parser and frame transform) and Android (`ProtocolTest`).
- ✅ Protocol spec in `protocol/PROTOCOL.md`; both sides tested against `protocol/golden.txt`.
- Found by the tests and fixed: a stray `MCAM`/`MCMD` inside garbage could be taken for a header,
  and the phone's command reader could crash on a read over 4 KiB.

---

## 7. Design language and assets

Goal: one consistent retro "Windows 7" look across the phone app, the PC tray/settings window, the
virtual camera's status frames and the installer. Decide it and draw the assets **before** building the
new UI (1.1 pause button, 4.1 dimmed screen, 5.1 tray icons, 5.2 settings window, 5.3 installer), so each
of those is built once, in the final style.

References: `inspo/`.

### 7.1 Decision: which "retro Windows"? ✅ Decided: A, Aero (Windows 7)
The reference images mix two different eras:

| Reference | Era | Look |
|---|---|---|
| `Windows 7 icons.jpg`, `Vista VS glass effect.jpg` | **Vista / 7 "Aero"** | Translucent glass title bars, soft blue gradients, glossy pill buttons, rich 3D icons, Segoe UI type |
| `windows 7 windows.jpg`, `more win7 inspo.jpg` | **Windows 95/98 "Classic"** | Flat gray (#C0C0C0) with hard 3D bevels, navy title bars, pixel-art icons, bitmap-style type |

Options:
- **A. Aero (Windows 7)** *(recommended)*: matches the stated "win7" goal. It looks premium and friendly,
  and it scales well to high-DPI phone screens.
- **B. Classic (Windows 95/98)**: stronger retro/meme character. It's easier to draw (flat bevels, pixel
  icons), but needs care to stay crisp at phone DPIs.
- **C. Aero shell + Classic easter eggs**: Aero everywhere, with Classic-style dialogs only for playful
  moments (e.g. the "No signal" frame looks like a 98-style "System message" box).

**Decision (2026-10-09): option A.** Everything uses the Windows 7 Aero look. The Classic 95/98 images
in `inspo/` are not used as a style source.

### 7.2 Style guide (S): `design/STYLE.md`
Written once, used by every UI. Starting values for Aero (approximate; refine during 7.4):

| Token | Value (start) | Use |
|---|---|---|
| Glass frame | translucent sky blue, about `#6FA8DC` at 60–70% with blur and a white inner highlight | Title bars, headers |
| Window body | `#F0F0F0` | Panels, dialogs |
| Button, normal | gradient `#F2F2F2 → #DDDDDD`, border `#707070`, radius 3 px | All buttons |
| Button, hover / focus | gradient `#EAF6FD → #BEE6FD`, border `#3C7FB1` | Phone: focus/pressed |
| Button, pressed | gradient `#C4E5F6 → #98D1EF`, border `#2C628B` | |
| Selection | fill `#CCE8FF`, border `#99D1FF` | Selected camera, list rows |
| Progress / "live" green | `#06B025` with a gloss highlight | Streaming indicator |
| Text | `#000000`, secondary `#6D6D6D`, link `#0066CC` | |
| Aurora background | blue-green gradient with soft light streaks | Phone background, installer banner |
| Type | Segoe UI look-alike. Android can't bundle Segoe UI, so use **Selawik** (Microsoft's open-source Segoe fallback, OFL), after checking its license | Phone app, frames |

It also defines spacing, corner radii, shadow/glow, icon sizes and states (normal, hover, pressed,
disabled), plus how much transparency/blur to use on Android (keep it cheap: blur only on static
backgrounds).

### 7.3 Asset list (M)
All drawn **originally, in the style of** Windows 7. No copied Microsoft icons, wallpapers, logos or fonts.

| Asset | Sizes / format | Where |
|---|---|---|
| App icon: glossy webcam, Win7-icon style | Android adaptive (108 dp fg/bg) + Play 512 px; Windows `.ico` 16/20/24/32/48/256 | Phone launcher, companion exe, installer |
| Tray status icons: disconnected / ready / streaming / paused / error | 16, 20, 24, 32 px (100–200% DPI) `.ico` | Windows tray (5.1) |
| Android notification icon | Single-color silhouette, 24 dp vector (Android forces monochrome) | Foreground-service notification |
| Status frames shown on the PC camera: "No signal", "Paused", "Phone locked" | 1920×1080 + 1280×720 NV12-friendly PNG | Virtual camera (1.1, 1.2) |
| Phone UI kit: glass header, Aero buttons, camera toggle, big pause/resume button, "live" pill, status card | Compose components + 9-patch/vector drawables | Phone app |
| Dimmed streaming screen: dark aurora with a small status readout | Vector/Compose | 4.1 |
| Settings window chrome: custom-drawn glass title bar + controls | Win32 custom draw or a small UI toolkit | 5.2 |
| Installer art: wizard banner + side panel | Inno Setup / WiX sizes | 5.3 |

### 7.4 Mockups before code (S)
Mock up the phone main screen, the dimmed streaming screen, the tray menu, the settings window and the
"Paused" frame in the chosen style. Approve those, then implement.

### Notes and risks
- **Tray menus:** Windows draws tray menus in the current Windows 11 theme. A fully retro menu needs a
  custom popup window instead of the native menu. That's more work, and native is more accessible.
  Suggest: retro icons in a native menu, with the retro look saved for the settings window.
- **Accessibility:** glossy gradients can hurt contrast. Keep text at WCAG AA contrast and keep the dark,
  dimmed screen readable.
- **Licensing:** Segoe UI, Windows wallpapers and Microsoft icon artwork can't be shipped. Everything in
  7.3 must be original or properly licensed.

---

## Suggested order

0. **7** Design language (Aero chosen): write the style guide, mock up, then draw the assets
1. **1.2** Pause when Windows is locked (privacy)
2. **1.1** Pause button on the phone
3. **3.1** Crop to fill, and **4.1** dim the screen while streaming
4. **2.1–2.3** Remaining reliability work
5. Everything else as needed
