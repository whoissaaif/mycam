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

Still untested: pause from the phone's **notification**, pause **surviving unplug/replug**, PC sleep and
resume, killing or crashing the companion mid-stream, two phones plugged in at once, and a phone that has
never been connected before (the first-use UAC flow).

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

### 2.2 Fewer UAC prompts on first use (S): ⏳
A new phone prompts twice: once at install, then again for WinUSB binding. Have the installer bind any
phone that is plugged in, and explain the one remaining prompt in the tray balloon.

### 2.3 Test the remaining scenarios (S–M): ⏳
See "Still untested" above, plus phone low-battery mode and Android killing the service under memory
pressure. Fix whatever breaks.

---

## 3. Picture quality and camera control

### 3.1 Crop to fill (S): ⏳
A portrait phone gives a narrow image with black bars. Add a "Crop to fill" option that center-crops to
16:9 (`DrawFittedNV12` in `pc/vcam/frame_transform.cpp`), with a toggle in the settings window.

### 3.2 Resolution and frame-rate options (M): ⏳
720p / 1080p / 4K, and 60 fps where the phone supports it. Add a protocol command to request a mode, and
advertise the matching media types from the virtual camera.

### 3.3 Camera controls (M): ⏳
Zoom, tap-to-focus, exposure, torch, and lens choice (ultrawide / telephoto). Phone UI first; exposing them
to PC apps through `IKsControl` is a stretch goal.

### 3.4 Lower latency (M): ⏳
Decode on the GPU (D3D11 / DXVA) instead of the CPU, cut extra frame copies, and add a latency
measurement (timestamp in the frame header → log).

---

## 4. Phone app

### 4.1 Dim the screen while streaming (S): ⏳
A dark aurora "streaming" screen keeps the phone cooler, saves battery and is more private. Show a warning
when the phone gets hot (`PowerManager.getCurrentThermalStatus`).

### 4.2 Branding and release signing (S): 🟡
- ✅ Real app icon (Aero webcam, adaptive + legacy + themed).
- ⏳ Proper package name instead of `com.example.mycam`. Changing it later means a separate install, so
  do it before sharing widely.
- ⏳ Release signing key. APKs are currently signed with the debug key: fine for sideloading, not for
  the Play Store.

---

## 5. PC app: ✅ done (1.0–1.1), except code signing

### 5.1 Tray icon that shows status (S): ✅
Aero webcam with a status badge: disconnected (grayscale), ready, streaming (green), paused (amber),
error (red). 16–256 px, DPI-aware.

### 5.2 Settings window (M): ✅
Aero window (Direct2D): glass title bar, live status with a LIVE pill, Pause / Resume, back/front camera,
mirror, start with Windows, reconnect, open log folder. Keyboard: Tab / arrows, Space / Enter, Esc.
- ⏳ Follow-up: the controls are custom-drawn, so screen readers can't see them (needs UI Automation).

### 5.3 Real installer (M): ✅, code signing pending
Inno Setup wizard with Aero artwork (`installer/`, built by `installer/build.ps1`). It installs UsbDk if
needed, registers the camera, offers "Start with Windows", and lists MyCam in Settings → Apps. Upgrades
close the running companion cleanly; the uninstaller asks before removing UsbDk.
- ⏳ **Code signing.** Needs a code-signing certificate; `mycam.iss` is ready for it (`/DSIGN`).
- Licensing: libusb is LGPL-2.1 and statically linked, so anyone given the binaries must also be able to
  get the source. The GitHub repo is private: if you share builds, share the source too (or make the repo
  public).

---

## 6. Project hygiene: ✅ done
- ✅ Git, with dev tools kept out of shipped builds (`-DMYCAM_BUILD_TOOLS=ON`).
- ✅ Unit tests: C++ (`pc/tests`: packet parser, frame transform, BGRA→NV12) and Android (`ProtocolTest`).
- ✅ Protocol spec in `protocol/PROTOCOL.md` (v2); both sides tested against `protocol/golden.txt`.

---

## 7. Design language and assets: 🟡 mostly done

### 7.1 Decision: ✅ A, Aero (Windows 7)
Everything uses the Windows 7 Aero look. The Windows 95/98 "Classic" images in the local `inspo/` folder
are not a style source. `inspo/` holds Microsoft artwork and stays out of the repository.

### 7.2 Style guide (S): 🟡
The tokens are implemented in code and shared by both apps: `app/.../ui/theme/Color.kt` and
`pc/companion/settings_window.cpp`.
- ⏳ Write `design/STYLE.md`: tokens, type ramp, spacing, components, do's and don'ts.

| Token | Value |
|---|---|
| Glass header / frame | `#C9DDF3` → `#A9C6EA` → `#8FB2DD`, edge `#3E5F8A`, white aurora sheens |
| Body / command area | `#FFFFFF` / `#F0F0F0` with a `#DFDFDF` top line |
| Main instruction / heading | `#003399` / `#1E3287` |
| Button, normal | two-tone `#F2F2F2 · #EBEBEB │ #DDDDDD · #CFCFCF`, border `#707070`, radius 3 |
| Button, pressed / selected | `#E5F4FC · #C4E5F6 │ #98D1EF · #68B3DB`, border `#2C628B` |
| Live green | `#8BE07A · #37C12B │ #06B025 · #3CCB47`, border `#0A7A1A` |
| Paused amber / error red | `#FFE482 → #D68000` / `#FF968C → #C41818` |
| Text | `#000000`, secondary `#5A5A5A`, link `#0066CC` |
| Type | Segoe UI (PC), Selawik (phone, OFL) |

### 7.3 Assets: 🟡
All drawn from scratch by scripts in `design/tools/` (shared library `aero_draw.ps1`).

| Asset | Status |
|---|---|
| App icon (Windows `.ico`, Android adaptive / legacy / themed) | ✅ |
| Tray status icons (5 states) | ✅ |
| Android notification icon (monochrome) | ✅ |
| Camera pictures: "Camera paused", "Waiting for the phone" | ✅ "Phone locked" isn't needed: a lock shows "Camera paused" |
| Phone UI kit (Compose: header, buttons, command link, badges, LIVE pill) | ✅ |
| Settings window chrome | ✅ |
| Installer art (wizard panel, header) | ✅ |
| Dimmed streaming screen | ⏳ with 4.1 |
| Play Store 512 px icon and feature graphic | ⏳ only if publishing to the Play Store |

### 7.4 Mockups: skipped
The UI was built directly in the chosen style and reviewed from screenshots instead.

### Notes and risks
- **Tray menu** uses the native Windows 11 menu (accessible); the Aero look lives in the icons and the
  settings window.
- **Accessibility:** keep text at WCAG AA contrast on the glossy surfaces.
- **Licensing:** no Microsoft artwork, wallpapers or fonts are shipped.

---

## 8. Publishing and releases: 🟡

- ✅ GitHub repo (private): https://github.com/whoissaaif/mycam (MIT license).
- ✅ Commit history uses a GitHub no-reply email; `inspo/` and raw screenshots are excluded.
- ✅ Release **v1.1.1** with `MyCam-Setup-1.1.1.exe` and `MyCam-1.1.1.apk`.
- ⏳ Release checklist: bump versions (`app/build.gradle.kts`, `installer/mycam.iss`), run
  `installer\build.ps1`, build the APK, tag `vX.Y.Z`, then `gh release create`.
- ⏳ Decide public vs. private before sharing (download links and the phone's "get the app" link only
  work for you while the repo is private).

---

## Suggested order

1. **2.3** Test the untested scenarios (cheap, and it finds the real bugs)
2. **2.1 / 2.2** Remaining reliability and first-use polish
3. **3.1** Crop to fill, and **4.1** dim the screen while streaming
4. **4.2** Package name and release signing, before sharing more widely
5. **7.2** Write `design/STYLE.md`
6. Everything else as needed (3.2–3.4, 5.2 accessibility, 5.3 code signing)
