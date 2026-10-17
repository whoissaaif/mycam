# MyCam Website: Design and Build Brief

Owner: Aiyan Ahmed. App: MyCam (turns a phone into a webcam for Windows 11).
Repo: https://github.com/whoissaaif/mycam (MIT).
Goal: a premium, non-"AI-template" site that turns visitors into users.

---

## 1. Concept and message

- **Theme:** viewfinder / game HUD, dark. Corner brackets, REC dot, focus box, tiny readouts.
- **Core message:** "Your phone is already a better camera than your laptop's. MyCam makes Windows see it as a webcam."
- **Idea:** loading is *focusing*. The page starts blurry and snaps sharp, which is also what the product does for your video.
- **Not the theme:** Windows XP is not the site theme. XP only appears inside the depiction of the app (see section 9) and optionally as an easter egg.
- Follow the five poster rules (section 3), not the poster's look.

## 2. Pages and visitor flow

| Page | Purpose |
|---|---|
| **Home** (long scroll) | Persuade, then download |
| **Get started** | Install steps for Android, iPhone, USB, plus troubleshooting |
| **Privacy** | Privacy policy and code signing policy (SignPath requires it on the site) |

Minimum is two pages (Home + Privacy). Three is the plan.

### Home sections, in order
1. **Hero:** one-line promise, Download button, small "Windows 11" label, live HUD readouts.
2. **Before / after:** draggable slider, laptop webcam vs phone through MyCam.
3. **How it works:** three steps (Install on PC, Connect phone, Pick "MyCam" in any app).
4. **Works in every app:** Zoom, Teams, Meet, OBS, Discord (text list or marquee; no logos unless allowed).
5. **Features:** pinned/accordion: USB, Wi-Fi, iPhone via Safari page, pause, front/back camera, resolution/fps/zoom/torch chosen on PC.
6. **See the app:** the live recreated screen (section 9).
7. **Install and trust:** installer ~12 MB, SmartScreen walkthrough, SHA-256, signing policy, what the installer adds.
8. **FAQ:** accordion.
9. **Download band + footer:** big wordmark, GitHub link, Privacy link.

## 3. The five rules as specs

| Rule | What it means here |
|---|---|
| Clear hierarchy | One huge headline per section, one supporting line, one action. Hierarchy comes from size and tone (white vs grey), not from many colors |
| Strong contrast | Light text on near-black; red only for the one thing to click or notice |
| Alignment | 12-column grid, everything snaps to it, left edges line up |
| White space | 128px between sections; never fill empty space just because it is there |
| Consistency | Same tokens, same type, same motion everywhere |

## 4. Look

### Colors
| Token | Hex | Use |
|---|---|---|
| ink | `#0B0C0E` | page background |
| panel | `#14161A` | cards, device frames |
| line | `#2A2D33` | borders, grid lines |
| text | `#ECEDEF` | headlines, body |
| muted | `#9AA0A9` | captions, secondary text (about 7.4:1 contrast) |
| rec | `#FF3B30` | the only accent: REC dot, primary button, focus box (about 5.5:1; under 5% of screen) |

### Typography (updated: wide display + Arial Bold)
- **Display:** Epic Pro, very wide, ultra-heavy, ALL CAPS. For the hero, section titles and the big "MyCam" wordmark. Line-height about 0.9, letter-spacing about -0.01em. Few words per line.
- **Everything small:** Arial Bold, slightly tight (about -0.02em). For nav, buttons, labels, captions, HUD readouts.
- **Body:** Arial regular, muted grey, 16-18px, line-height 1.5.
- **License check:** confirm a web-font license for Epic Pro. Free stand-in: **Anybody** (Google Fonts) at max width, weight 900.
- **Font stack for Arial:** `Arial, Arimo, Helvetica, sans-serif` (Arimo is a free metric-compatible fallback).
- Tone hierarchy: first words white, the rest muted grey, in the same headline.
- Caption row: small uppercase Arial Bold labels like `01 / HOW IT WORKS`.

### Layout
12 columns, 24px gutter, 1280px max width, 8px spacing unit, 128px section padding.

### Framing marks
Viewfinder corner brackets on hero and device frames. Small REC dot with blinking timecode. Focus box that snaps to the hovered element.

### Pixel art
Pixel elements (camera icon, cable, loading, 404) are drawn on a grid with `image-rendering: pixelated` and are always animated with `steps()` timing at 8-12 fps.

## 5. Motion and effects

- Easing `cubic-bezier(.625,.05,0,1)`. Durations 200 / 500 / 900 ms.
- Smooth scroll with Lenis. Scroll-driven reveals with GSAP + ScrollTrigger.
- **Autofocus cursor:** custom cursor that looks like a focus box; locks onto buttons and links.
- **Pixel-to-sharp:** key images load as chunky pixels and resolve to sharp (canvas pixelation).
- **Before/after slider:** drag to reveal phone quality vs laptop webcam.
- **One 3D phone:** a single CSS 3D or GLB phone, rotating with scroll. Drop it if no good model.
- Max **three** smooth effects visible per screen.
- Avoid: parallax everywhere, floating gradient blobs, glassmorphism, stock illustrations, emoji icons, generic "3 cards with icons" rows.
- Respect `prefers-reduced-motion` (turn effects off, keep layout).
- Ambient loops only run while on screen (IntersectionObserver).

## 6. Easter eggs

- Click the pixel camera: it takes a "photo" with a flash and shutter sound (muted by default).
- Console message for developers visiting.
- Optional Konami code: flips the whole site to a Windows XP Luna skin for a few seconds.

## 7. Loading page and 404

### Loading ("loading is focusing")
- Black screen, a small viewfinder with a pixel focus ring that hunts back and forth.
- Percent counter in Arial Bold. At 100% the ring locks (green-to-red REC flash) and the page snaps from blurry to sharp.
- Max 1.5 seconds; skip entirely on repeat visits and with reduced motion.

### 404 ("No signal")
- Pixel phone and pixel PC with a cable that is unplugged. The plug twitches.
- Headline in Epic Pro: `NO SIGNAL`. Line below: "This page isn't connected."
- One button: Back to home.

## 8. Conversion checklist

- [ ] One primary action per screen: Download for Windows
- [ ] Stable link: `https://github.com/whoissaaif/mycam/releases/latest/download/MyCam-Setup-latest.exe`
- [ ] Show version number and file size next to the button
- [ ] SmartScreen walkthrough ("More info" then "Run anyway") until signing is live, plus SHA-256
- [ ] Mobile visitors: a phone can't install a Windows app, so show "Send link to your PC" / "Copy link"
- [ ] Mac / Windows 10 visitors: say plainly what is supported
- [ ] Windows N editions: note the Media Feature Pack
- [ ] Short demo loop near the top
- [ ] SEO and Open Graph image
- [ ] Accessibility: contrast, keyboard focus, alt text, reduced motion
- [ ] Analytics decision (none, or privacy-friendly)
- [ ] Launch plan: Show HN, Reddit, LinkedIn

## 9. Showing the app without screenshots

There is really one app screen, shared by the PC app, the Android app and the iPhone web page. Instead of screenshots, build **one live HTML/CSS recreation** and show it in three frames (Windows XP window, Android phone, iPhone Safari), with a tab to switch. Label it "Illustration of the app".

- **Look of the real app:** Windows XP Luna style (blue gradient title bar, beige / light-blue panels, glossy buttons, green play orb, orange pause orb).
- **States that cycle:** Not connected, then Streaming, then Paused. A pixel cable plugs in, status flips, preview goes live, pause orb reacts on hover.
- **Use the app's exact words:**
  - Android: "MyCam", "Not connected", "Plug this phone into a PC running MyCam.", "Pause the camera", "Turns the camera off until you resume, here or on the PC.", "Camera", "Back camera" / "Front camera", "The camera turns on only while an app on the PC is using it."
  - iPhone page: "Paused", "The camera is off. Apps see a "Camera paused" picture.", "Resume the camera", "The camera turns back on if an app on the PC still uses it.", "Resolution, frame rate, zoom and torch are chosen on the PC.", "Keep it running" tips, "Dim the screen".
- **Still needed:** a sharp screenshot of the connected/streaming state (Android and iPhone) and of the PC app window.
- **Icons:** Android shows a chrome webcam, iPhone a camera. Pick one with saif; it becomes the logo mark and favicon. Make sure the orbs and icons are original art, not Microsoft's icon sets.

## 10. Open items

### Needs saif before publishing
- [ ] Stable download filename on GitHub Releases
- [ ] Privacy policy text and data-handling answers (what is stored; device token hash stays on the PC)
- [ ] Code signing policy wording and roles (SignPath)
- [ ] Which app icon is the official one

### Needs Aiyan
- [ ] Epic Pro license, or confirm Anybody as the stand-in
- [ ] Real before/after clips (laptop webcam vs phone through MyCam)
- [ ] 3D phone model (GLB) or drop the 3D phone
- [ ] Hosting and domain: GitHub Pages or Vercel; check mycam.app / .dev / getmycam.com, or use whoissaaif.github.io/mycam
- [ ] Tool to build the site with

## 11. Build notes

- **Stack:** plain HTML + CSS + JS (or Astro/Next if preferred). GSAP 3.12.5 + ScrollTrigger (cdnjs), Lenis 1.1.13 (jsdelivr). No heavy frameworks needed.
- **GSAP gotcha:** don't set an initial CSS `transform: translateY(%)` and then animate `yPercent`. Use `gsap.set` + `to`/`fromTo` with `yPercent`.
- **Hosting:** GitHub Pages (separate branch) or Vercel. Installer served from GitHub Releases.
- **Performance:** fonts self-hosted and subsetted, images in AVIF/WebP, no layout shift, lazy-load below the fold.

---

## 12. Paste-ready prompt for an AI builder

```
Build a dark, premium marketing site for "MyCam", a free open-source Windows 11 app that turns a phone (Android via USB/Wi-Fi, iPhone via a Safari page and QR code) into a webcam.

Pages: Home (long scroll), Get started, Privacy.

Theme: camera viewfinder / game HUD. Near-black background (#0B0C0E), panels #14161A, lines #2A2D33, text #ECEDEF, muted #9AA0A9, ONE accent red #FF3B30 (under 5% of the screen). Corner brackets, REC dot, tiny readouts.

Type: huge wide ultra-heavy ALL CAPS display font (Epic Pro, or Anybody at width 150 / weight 900 as a free stand-in), line-height 0.9. Arial Bold (stack: Arial, Arimo, Helvetica, sans-serif) with -0.02em tracking for nav, buttons, labels, captions. Arial regular in muted grey for body. Hierarchy by size and tone (white first words, grey rest).

Layout: 12-column grid, 24px gutter, 1280px max, 8px unit, 128px section padding. Strong alignment and white space; one action per screen.

Home sections: Hero (promise + Download for Windows + HUD readouts) / Before-after drag slider / How it works (3 steps) / Works in every app / Features accordion / "See the app" live recreated screen / Install and trust (SmartScreen steps, SHA-256, signing policy) / FAQ / Download band + giant wordmark footer.

Motion: Lenis smooth scroll, GSAP ScrollTrigger reveals, easing cubic-bezier(.625,.05,0,1), durations 200/500/900ms, max three smooth effects per screen. Custom autofocus-box cursor that locks onto links. Images resolve from pixelated to sharp. All pixel art animated with steps() at 8-12 fps. Respect prefers-reduced-motion.

"See the app": one live HTML/CSS recreation of the app screen (Windows XP Luna style: blue gradient title bar, glossy buttons, green play / orange pause orbs) in three frames (XP window, Android phone, iPhone Safari), cycling Not connected -> Streaming -> Paused. Use the app's exact copy. Label "Illustration of the app".

Loading page: black screen, pixel focus ring hunting, percent counter, locks on 100% and page snaps from blur to sharp; max 1.5 s, skipped on repeat visits.
404: "NO SIGNAL" with pixel phone and PC and an unplugged cable that twitches; button back home.

Easter eggs: clicking the pixel camera takes a flash "photo"; console message for devs; Konami code flips the site to XP skin briefly.

Conversion: download button with version and size, stable link https://github.com/whoissaaif/mycam/releases/latest/download/MyCam-Setup-latest.exe, "Send link to your PC" for mobile visitors, honest support note for Mac / Windows 10, SmartScreen walkthrough, Open Graph image, accessible contrast and keyboard focus.

Avoid: gradient blobs, glassmorphism, stock illustrations, emoji icons, generic three-icon card rows, parallax everywhere.
```
