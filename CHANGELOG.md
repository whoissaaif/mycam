# Changelog

Android app and PC companion share one version (SemVer: PATCH = fixes, MINOR = features, MAJOR = breaking
or protocol changes). Releases with their installers: https://github.com/whoissaaif/mycam/releases

## 1.6.0 — Linux (preview)

- **Linux PC companion, a preview.** `mycam_1.6.0_amd64.deb` for Ubuntu 24.04 (and similar): a background
  service that shows your phone as the **MyCam** camera in Zoom, OBS, Discord and browsers, through
  v4l2loopback. The phone camera only runs while an app is watching. A tray icon shows the status with the
  Windows tray's icons and menu (pause, camera, quality, mirror, fill, connect an iPhone, start at login).
  Tested with an iPhone over Wi-Fi; Android (USB and Wi-Fi) is built in but not tested on Linux yet. No
  settings window yet. Guide: `pc/linux/README.md`. (#28, #30, #31, #32, #33, #36, #37, #57)
- **Windows: the About page shows the right version** (it said 1.4.0 since 1.4.0); the build now reads it
  from the app's version, like the installer. (#56)
- **Android: the phone sleeps again after a Wi-Fi session.** The wake lock is released when a link ends,
  not only when the app closes. (#40)
- **Android: 4K is only offered once the camera reports it**, instead of on every phone until its camera
  first opened. (#53)
- **Android: zoom and brightness stay in range** before the camera has opened once. (#52)
- **Android: a Privacy Policy link** in Settings → About. (#50)
- **PC: a failing decoder no longer floods the log**; it retries every 2 s instead of on every frame. (#51)
- **Windows: MyCam is in the Start menu**, so Windows search finds it. Opening it while MyCam is already in
  the tray shows its window instead of doing nothing. (#73)
- **A contact address, support@mycam.dev**, in the About pages of both apps and in the README. (#72)
- **No more pairing questions out of the blue.** Opening MyCam on the phone no longer pops up "Pair with
  <PC>?" because some PC on the network is searching. A new PC can only ask to pair for 2 minutes after you
  open Find devices (or tap a PC there), and "Find phones on Wi-Fi" on the PC only reconnects phones it is
  already paired with. (#69)
- **Pulling the cable carries on over Wi-Fi.** With a phone paired over Wi-Fi and "Use over Wi-Fi" on, the
  stream continues over Wi-Fi within a few seconds of unplugging, even with "Find phones on Wi-Fi" off on
  the PC (it looks for paired phones for 30 s after the cable goes). The phone also turns its saved Wi-Fi
  mode back on when plugging in starts the app. (#70)
- **Plugging in a cable no longer drops a working Wi-Fi stream** when the cable can't be opened (no
  driver yet, access denied). Both sides stay on Wi-Fi until the cable actually works, then switch; a
  cable that fails isn't tried again until it is plugged in again. (#67)
- **Windows: USB works with USB debugging on.** On PCs that have the Samsung USB driver (installed with
  Smart Switch or for any Samsung phone), the phone's accessory interface got the MTP driver and MyCam never
  connected over the cable. MyCam now recognises the accessory interface however Windows names it, swaps a
  wrong MTP driver on it for WinUSB (only that interface), and connects even while adb is using the phone.
  If switching a phone to MyCam's USB mode fails right after plug-in, it is tried 3 times. (#66)
- **Windows: a "Show preview" switch** under the preview on the Camera page (on by default, remembered).
  The preview keeps the phone streaming while the page is open; switched off, the window never asks for
  video, so the phone camera only runs while an app uses it.

No protocol changes: 1.6.0 phones and PCs work with 1.5.x.

## 1.5.1 — fixes

- **Windows 11: no more black camera.** A companion that started before any app opened MyCam (at login,
  say) fell back to the Windows 10 hand-off and never switched, so apps showed black and the phone camera
  never started. It now switches to the Windows 11 camera as soon as it appears. (#43, #44; came in 1.5.0)
- **USB phones with USB debugging on** connect again: WinUSB is now bound to the accessory interface of
  the phone's composite device. (#42)
- **4K keeps up:** H.264 is decoded on the GPU (DXVA) when the PC supports it. (#41)
- **Android: pairing keys stay out of backups and phone-to-phone transfers**, so a restored or copied phone
  doesn't carry this PC's pairing. Existing keys move out of the backed-up settings once. (#35)

No protocol changes.

## 1.5.0

Windows 10 support: a DirectShow camera next to the Windows 11 Frame Server one; the installer registers
whichever the PC supports.
