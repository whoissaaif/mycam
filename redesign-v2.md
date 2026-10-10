# MyCam redesign v2: matching the designer's mockup

An analysis of the mockup the owner's designer supplied (`design/previews/designer-mockup.png`), compared
with what MyCam has today on branch `xp-redesign` (1.4.0-beta4), and the work needed to match it.

Companion documents: [redesign.md](redesign.md) (the XP spec we built) and
[design/STYLE.md](design/STYLE.md) (the token rulebook). Where this file and those disagree, **this file
wins** for anything the mockup actually shows; redesign.md still governs everything the mockup is silent
about.

---

## 1. What the mockup is

Ten screens on a Bliss-style hills background: four Windows windows and six phone screens.

**Windows:** 1. Home (connect a device) · 2. Device discovery · 3. Pairing · 4. Camera control with a live
preview.

**Phone:** 1. Splash · 2. Connection · 3. Scan for devices · 4. Pairing · 5. Live preview ·
6. Camera settings.

### 1.1 The single most important observation

The mockup is **XP on the outside, modern flat on the inside**.

- **Kept from XP/Luna:** the blue gradient title bar, the three square caption buttons with the red close
  box, the hills-and-sky imagery, the overall friendliness.
- **Not XP at all:** the window interior. It uses a left **sidebar nav**, white **rounded cards** with soft
  shadows, flat line icons, **dropdowns** (combo boxes), **toggle switches**, a **slider**, and a single
  blue accent (`#1E6FE8`-ish) for every action. There are no beige `#ECE9D8` surfaces, no task-pane
  gradient, no green check marks, no glossy bevelled buttons, no green progress bar.

So this is not "more XP". It is **XP chrome wrapped around a clean modern app**. That is a deliberate and,
in my view, good choice: the XP shell carries the personality, while the controls stay legible and modern.
But it means a large part of what we built in the last pass changes.

### 1.2 What this costs

| Built in this pass | Mockup's position |
|---|---|
| XP task-pane groups (collapsible, lavender) | Replaced by sidebar nav + white cards |
| Green check boxes and radio dots | Replaced by toggle switches and dropdowns |
| Glossy bevelled XP push buttons | Replaced by flat blue/white rounded buttons |
| Beige `#ECE9D8` window body | Replaced by white / very light grey |
| Green segmented progress bar | Not shown anywhere; a radar animation appears instead |
| Zoom presets (0.6× / 1× / 2× / 5×) | Replaced by a continuous slider |
| Phone Now/Settings tabs | Replaced by a 3-item bottom nav (Connect / Camera / Settings) |
| **No preview on the phone** (owner's earlier decision) | **The phone shows a live preview** (screen 5) |

The last row is a direct reversal of a decision the owner made earlier in this project, so it needs a
decision rather than an assumption. See §6.

---

## 2. Windows app: what changes

### 2.1 Navigation: sidebar replaces the task pane

The mockup's window is **two columns**: a ~150 px white sidebar and a content area.

- Sidebar items: **Home · Devices · Camera · Settings · About**, each a line icon plus a label, with the
  selected row as a filled blue rounded pill with white text.
- The content area shows one page at a time, with a page title and a subtitle
  ("Connect a Device / Find and connect to your phone").

Today we have one scrolling task pane with every group stacked in it. The change is structural: the window
becomes a small **page router**, with the element model and the UI Automation tree rebuilt per page.

**What this fixes:** our window is dense. Five groups plus a preview plus controls in 720 × 500 is a lot.
Pages make each screen calm.

**What this costs:** more clicks to reach a setting, and the UIA tree changes shape per page (the provider
already rebuilds from `ui_model.h`, so this is mostly layout work).

### 2.2 Page 1, Home

Two large white cards, each with an icon, a title, a one-line description and a primary button:
- **USB Connection** — "Connect your phone via USB for best performance." → `Scan for USB Devices`
- **Wi-Fi Connection** — "Both devices must be on the same Wi-Fi network." → `Scan for Devices`

This is a genuinely better first-run surface than ours, which drops the user straight into status plus every
control. New work: a USB scan action (we only ever scan Wi-Fi; on USB we wait for a plug-in, so "Scan for
USB Devices" maps to a re-enumerate plus the existing `Reconnect phone`).

### 2.3 Page 2, Devices

- Header "Available Devices" with a **Refresh** button at the right.
- Rows: a phone glyph, the device name in bold, the IP beneath it, a **transport icon** (Wi-Fi arcs or the
  USB trident) and a blue **Connect** button.

We built almost exactly this in "Scan for phones" (name, IP, status, Connect link). Changes needed:
a dedicated page, the transport icon, Connect as a **button** rather than a link, and USB devices listed in
the same list.

### 2.4 Page 3, Pairing

A centred column: a phone illustration, "A pairing request has been sent to your phone. Make sure the code
below matches on both devices.", the **code in a large bordered box**, and a **Cancel** button.

Ours is a separate modal dialog window. The mockup makes it a **page inside the main window**. The page
form is calmer and avoids the owner-window problems we already had to fix. I recommend keeping a dialog
**only** when the main window is closed (the pairing request must still be reachable from the tray), and
otherwise showing this page.

Our countdown bar is not in the mockup. Keep it: a 60-second window with no visible timer is worse. Render
it as a thin flat blue determinate bar under the code, not the XP green chunks.

### 2.5 Page 4, Camera

- **Left:** the live preview, large, with three dark pill chips along the bottom: `1920 × 1080`, `60 FPS`,
  `Back Camera`.
- **Right:** a settings column — **Camera** (dropdown), **Resolution** (dropdown), **Frame Rate**
  (dropdown), **Zoom** (slider with `1.0x` to the right), **Auto Focus** (toggle), **Torch / Flash**
  (toggle).

Changes from ours: radio groups → dropdowns; the zoom −/+/preset row → a slider; check boxes → toggles; our
frosted LIVE chip → three flat dark chips.

**Keep from ours, because the mockup cannot show it:** every option must still come only from what the phone
reports. A dropdown must **omit or disable** entries the phone cannot do (4K on a phone without it, 120 fps
at a quality that cannot do it) and must carry the one-line reason we already compute
("120 fps works at 720p on this camera"). A dropdown hides that reason, so it goes under the control.

The mockup has no Pause control on this page. **Pause must stay** — it is the privacy promise of this
product. Put it under the preview, or in the sidebar as a persistent item.

### 2.6 Window chrome

The mockup shows **minimise, maximise and close**. Ours has no maximise and is fixed at 720 × 500. The
mockup's window is wider (roughly 4:3) and must stay resizable if maximise exists. That means a real
resizable layout, which the layout helper already supports but the fixed `kW`/`kH` constants do not.

---

## 3. Phone app: what changes

### 3.1 Navigation: a 3-item bottom bar

**Connect · Camera · Settings**, with the active item in blue. Ours has two top tabs (Now / Settings) and a
bottom utility bar. The mockup's split is clearer: *connecting* and *using* are different jobs.

### 3.2 Screen 1, Splash

Full-bleed hills, the app icon, "MyCam", "Turn your phone into a webcam", and a blue **Get Started** button.
We have no splash. This replaces our 3-card first-run guide as the first impression; the cards can remain
behind Get Started, or disappear entirely in favour of the Connect screen, which is self-explanatory.

Note: the owner has just asked that first-run never repeat. A splash that shows **every** launch is fine
(it is 1 s of branding), but it must not block, and the setup cards must still appear only once.

### 3.3 Screen 2, Connect

"Connect to PC / Choose how you want to connect.", then three tappable rows:
- **Scan QR Code** — "Pair using code from PC"
- **Find Devices** — "Search on the same Wi-Fi"
- **USB Connection** — "Connect via USB cable"

**Scan QR Code is a new feature.** It is the single biggest functional addition in the mockup: the PC shows
a QR code, the phone's camera reads it, and pairing completes without typing or comparing six digits. See
§5.

### 3.4 Screen 3, Scan for devices

A **radar animation**: concentric rings with a sweep and dots for found devices, "Scanning for devices…",
the hint "Make sure your PC app is open and both devices are on the same Wi-Fi network.", then result rows
(`DESKTOP-SAIF`, `192.168.1.12`).

Ours shows the green marquee instead. The radar is friendlier and reads as "looking around", which is what
discovery is. It also honours reduced motion poorly, so keep a static fallback.

### 3.5 Screen 4, Pairing

A monitor illustration, "Pair with this device?", the device name, "Make sure the code below matches on your
PC.", the code, then **Reject** (plain) and **Accept** (blue filled).

Ours is close. Differences: an illustration instead of a shield, "Reject/Accept" instead of
"Don't allow/Allow", and no countdown shown. Keep the countdown.

**Wording:** "Don't allow / Allow" is the better pair. "Reject" sounds like the device is at fault, and
Android's own permission language is allow/don't allow. I would keep ours and adopt everything else.

### 3.6 Screen 5, Live preview

The camera preview fills the screen, with a **Front Camera | Back Camera** segmented control and three stat
tiles beneath it: `1080p / Resolution`, `60 / FPS`, `1.0x / Zoom`.

This reverses the "no preview on the phone" decision. See §6.

### 3.7 Screen 6, Camera settings

Rows with the control on the right: **Resolution** (dropdown), **Frame Rate** (dropdown), **Zoom** (slider +
`1.0x`), **Auto Focus** (toggle), **Torch / Flash** (toggle). Same mapping as the PC page, so the two apps
finally use identical controls for identical settings — which is what §4 of redesign.md asked for and we
only half-achieved.

---

## 4. Visual system: the deltas to apply

| Token / component | Today | Mockup |
|---|---|---|
| Window body | `#ECE9D8` beige | White `#FFFFFF`, page background near-white `#F5F7FA` |
| Card | flat lavender group body | White, radius ~10, 1 px `#E3E8EF` border, soft shadow |
| Primary action | glossy XP button | Flat blue `#1E6FE8` → `#1760D0`, white text, radius 6 |
| Secondary action | glossy XP button | White fill, `#D0D7E2` border, dark text |
| Selection in nav | n/a | Blue filled pill, white text + icon |
| Choice of value | green radio group | **Dropdown** (combo box) |
| On/off | green check box | **Toggle switch** (blue when on) |
| Continuous value | −/+ buttons and presets | **Slider**, value label to the right |
| Progress | green XP chunks | Not shown; use a thin flat blue bar, and a radar for scanning |
| Preview overlay | frosted glass chips | Flat dark pills at ~70 % black, white text |
| Icons | glossy XP badges | Flat line icons, 1.5 px stroke, in the blue accent or grey |
| Title bar | Luna gradient | **Unchanged** (keep ours) |
| Caption buttons | ours | **Unchanged** (keep ours, but add maximise) |

**Typography:** the mockup uses a plain modern sans (Segoe-like). Our Tahoma/Trebuchet choice is the XP
tell. Recommendation: keep **Trebuchet MS Bold for the title bar only**, and move body text to **Segoe UI**
on the PC. On the phone, DejaVu Sans stays (it is already neutral enough and is licence-safe).

**Accessibility, unchanged and non-negotiable:** every pair must stay at WCAG AA. White on `#1E6FE8` is
4.6 : 1, which passes. Toggle switches need a visible off state (grey track with a border), not just colour.
Dropdowns and sliders must be keyboard operable and exposed through UI Automation
(`ExpandCollapse` + `Selection` for a dropdown, `RangeValue` for a slider, `Toggle` for a switch).

---

## 5. New features the mockup implies

| Feature | Where | Effort | Notes |
|---|---|---|---|
| **QR code pairing** | Phone screen 2, PC pairing page | **L** | The PC renders a QR of its address, port and a pairing token; the phone's camera decodes it. Needs a QR encoder on the PC and a decoder on the phone (ML Kit barcode, or ZXing to avoid a Play dependency). It also needs a protocol note, since the QR must carry enough to skip the six-digit compare safely: the PC's public-key fingerprint, not just the IP. **This is a security-relevant change and must be designed in PROTOCOL.md before it is built.** |
| **USB device scan** | PC Home + Devices | S | Re-enumerate rather than waiting passively. |
| **Sidebar page router** | PC | M | Including the UIA tree per page. |
| **Dropdowns, toggles, sliders** | Both apps | M | New components in both kits, with keyboard and UIA support. |
| **Splash screen** | Phone | S | |
| **Radar scan animation** | Phone | S | With a static reduced-motion fallback. |
| **Stat tiles** | Phone preview | S | |
| **Phone live preview** | Phone screen 5 | M | Pending the §6 decision. |
| **Resizable / maximisable PC window** | PC | M | Only if maximise is kept. |

---

## 6. The one decision I need from the owner

**The mockup puts a live preview on the phone. You decided earlier not to have one.**

Arguments for adding it, as the designer drew it:
- You can frame the shot while the phone is on a tripod, without walking to the PC.
- Screen 5 is otherwise a mostly empty screen.

Arguments for your original decision:
- The phone runs hot during long calls; a preview surface adds load exactly when heat matters.
- The dim screen exists to keep the phone cool and private; a preview is its opposite.
- The PC preview already shows what apps receive, which is the authoritative view.

**My recommendation:** add it, but make it **off by default and auto-off when the screen dims**, with a
setting "Show preview on this phone". That satisfies the mockup without giving up the thermal and privacy
work. It also matches the earlier recommendation in redesign.md §12 question 1, which went the other way
only because there was no preview at all then.

---

## 7. New artwork supplied

Two images the owner supplied and states are free to use. Both are now in the repo:

| File | Use |
|---|---|
| `design/assets/source/hills-photo.png` (1536 × 1024) | The new background: splash, first run, mockup backdrop, and the base for the camera pictures |
| `design/assets/source/camera-paused-reference.png` (1536 × 1024) | The new "Camera paused" picture: the same hills with a frosted glass card, a crossed-out camcorder glyph and "CAMERA PAUSED" |

**How these change the art pipeline.** Today `make_status_frames.ps1` **paints** the hills procedurally,
because the earlier reference photo had no known licence. With a photo that is free to use, the scripts
should composite over the photo instead, which looks far better than anything drawn with GDI+ gradients.

Work needed:
1. Record the source and licence of both images in `design/assets/source/README.md`. The owner states they
   are free to use; the actual licence name and origin should be written down before release, because the
   repo is MIT and the images ship inside the APK and the installer.
2. Rework `make_status_frames.ps1` to composite: downscale the photo to each target size, blur it slightly,
   draw the frosted card, then the glyph and the text.
3. Render the frames at the sizes the apps need (1280 × 720 today; 1920 × 1080 is worth adding).
4. Match the supplied paused screen: a **crossed-out camcorder** glyph rather than our round amber pause
   badge, "CAMERA PAUSED" in caps, letter-spaced, centred. Note the supplied image has **no** subtitle and
   no wordmark; I recommend keeping a small "MyCam" wordmark bottom-left so a viewer on a call knows which
   app paused.
5. The waiting picture keeps the same treatment with its own text and the progress bar.
6. Use the photo for the phone splash and first-run background, replacing the Compose-drawn hills. The
   night Dim screen stays drawn (a dark photo would be wasted bytes).

**Size:** the photo is 2.7 MB as PNG. Ship **JPEG** at the sizes needed (a 1920 × 1080 JPEG at quality 85 is
roughly 250 KB) and keep the PNG as the source only. The APK is currently ~2.8 MB, so this matters.

---

## 8. Fix: the transparent flash when the PC window opens

**Owner-reported, and now fixed on `xp-redesign`.**

**Cause.** The window is created with `WS_EX_NOREDIRECTIONBITMAP` and draws through DirectComposition, so it
has no surface of its own: until the swap chain holds a frame, there is nothing to show. The code called
`ShowWindow` immediately after creating the window and only painted on the first `WM_PAINT`, so for one or
two frames an empty, see-through window was on screen. On top of that, the opening animation faded the
content in from **zero** opacity over 150 ms, which stretched the empty look into something clearly visible.

**Fix** (`settings_window.cpp`):
1. Draw and present one complete frame **before** `ShowWindow`.
2. Remove the 150 ms fade-from-transparent open animation. The DWM already animates a window opening, so
   the extra fade only delayed the first readable frame.

The window now appears fully drawn on its first frame. The same pattern applies to the pairing dialog if it
is ever seen to flash.

---

## 9. Recommended order

| Phase | Work | Size |
|---|---|---|
| **0. Done** | The open-flash fix (§8); the supplied images added to the repo | — |
| **1. Art** | Licence note; composite the camera pictures over the photo; new paused design; splash and first-run background; JPEG sizing | M |
| **2. Component kit** | Dropdown, toggle switch, slider, flat buttons, white cards, dark preview chips, flat blue progress — on **both** platforms, with keyboard and UIA support | M |
| **3. Windows shell** | Sidebar page router; Home, Devices, Camera pages; pairing as a page; maximise/resize if kept | L |
| **4. Phone shell** | Bottom nav; splash; Connect screen; radar scan; stat tiles; settings rows | M |
| **5. Preview on phone** | Only after the §6 decision | M |
| **6. QR pairing** | PROTOCOL.md design first, then PC encoder and phone decoder | L |

Phases 1 and 2 are worth doing regardless of how much of the shell changes, because they are what makes the
app *look* like the mockup. Phase 3 is the largest single piece.

---

## 10. What I would push back on

Honest notes, since the mockup is a design and not a specification:

1. **Pause is missing** from every mockup screen. It is the product's privacy promise and must be visible on
   both apps, not buried.
2. **No status surface.** The mockup never shows "Streaming / Paused / Not connected". Our status model is
   the one thing users read at a glance, and it should stay on the PC Home page and the phone Connect
   screen.
3. **Dropdowns hide capability.** Radio groups made "this phone cannot do 4K" visible at a glance. With
   dropdowns, keep the unavailable entries visible but disabled, with the reason underneath.
4. **"Reject"** should be "Don't allow", to match Android's own language.
5. **The radar animation** must have a reduced-motion fallback.
6. **Maximise** on a fixed-layout window is a trap. Either make the layout genuinely resizable or drop the
   maximise button.
