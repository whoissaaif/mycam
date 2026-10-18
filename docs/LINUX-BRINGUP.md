# Linux bring-up: read this first

**Audience: a Claude Code session starting on Ubuntu, with no memory of the Windows sessions.**
Claude Code stores transcripts per machine and per project path, so nothing carries over from the
Windows work. This file is the handoff. Read it before touching anything.

Written 2026-10-18 on the Windows side, by the session that prepared this branch.

---

## 1. Where you are

| | |
|---|---|
| Repo | `github.com/whoissaaif/mycam` — public, MIT |
| Branch | `linux` (already exists on the remote, branched off `main` at `1355670`) |
| Who is working | Aiyan. saif owns the Windows build, the releases and the website |
| Goal | **Linux support, phase 1**: a headless daemon streaming an Android phone over USB into a `v4l2loopback` device |

MyCam turns a phone into a webcam. Android has an app; an iPhone uses a web page the PC serves.
The wire protocol, the Android app and the iPhone page are **platform-neutral and must not change**.

### Platform roadmap

| Platform | Status |
|---|---|
| Windows 11 | shipping (1.4.3) |
| Windows 10 | **implemented** — `pc/vcam-dshow/`, a DirectShow camera; verified in Discord on real hardware, Zoom/OBS/browser/upgrade-install still untested |
| **Linux** | **this work** — planned, not started |
| macOS | future, not designed |

---

## 2. Read these, in this order

1. **`CLAUDE.md`** — the repo's rules. Non-negotiable, and it changed recently: the project is now
   PR-based (see §7).
2. **`docs/LINUX-SUPPORT.md`** — saif's plan. **It is gitignored**, so it may be missing from your
   clone. If it is, ask for it before starting; §4 of this file summarises it but is not a substitute.
3. **`IMPROVEMENTS.md` §13** — the one-paragraph version of the same plan.
4. **`TASKS.md`** — the Linux line under "Now", plus everything else in flight.
5. **`docs/ARCHITECTURE.md`** — how the Windows build fits together. The Linux daemon mirrors it.

---

## 3. Three things to settle before writing code

Do not start phase 1 until these are answered. The first two need saif.

1. **Is `docs/LINUX-SUPPORT.md` in your clone?** If not, get it. It has the phased work items and the
   definition of done.
2. **The GUI question (§5 of saif's doc) is already answered.** He asks whether Linux keeps the
   Windows XP Luna look or goes native, and recommends deferring. **Aiyan's answer is: same UI as
   Windows.** That does not change phases 1–3 (none of them have a GUI), but it turns phase 4 from
   "decide later" into known, large scope. Confirm with saif so §5 can be closed in the doc.
3. **Does the virtual camera actually work on this machine?** See §5 — ten minutes, no code, and it
   validates or kills the whole approach.

---

## 4. The plan, in short

saif's phasing, which is the right order — cheapest and most useful first:

| Phase | What | Covers |
|---|---|---|
| **1** | Headless CLI daemon, **USB only**. Switch the phone to accessory mode, run the existing `SessionLoop`, decode H.264, write frames into `v4l2loopback`. No GUI. | Zoom, OBS, Chrome/Meet on Linux — most of the value |
| 2 | Wi-Fi transport; pairing storage moves from the registry to `~/.config/mycam/` | Android wirelessly |
| 3 | iPhone web server; `tls_server.cpp` from Schannel to OpenSSL | iPhone / iPad |
| 4 | GUI — tray, settings window, pairing dialog | after 1–3, and after the §5 decision |

**Phase 1 is the launch-blocking scope.** Everything else can follow.

---

## 5. Do this first, before any code

The single biggest unknown is whether `v4l2loopback` is actually seen by the apps people use. Answer
it with no MyCam code at all:

```bash
sudo apt install v4l2loopback-dkms v4l-utils ffmpeg
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="MyCam" exclusive_caps=1
v4l2-ctl --list-devices                      # expect /dev/video10 labelled "MyCam"

ffmpeg -re -f lavfi -i testsrc=size=1280x720:rate=30 -pix_fmt yuv420p -f v4l2 /dev/video10
```

With that running, check whether **MyCam appears and shows the pattern** in:

- [ ] Chrome or Chromium (any `getUserMedia` test page)
- [ ] Firefox
- [ ] OBS Studio
- [ ] Zoom, if installed

`exclusive_caps=1` matters: without it the device advertises capture capability even when nothing is
writing, and browsers list a camera that produces nothing.

**If this fails, stop and report.** It means v4l2loopback is the wrong backend and PipeWire has to come
sooner than planned — a plan-level change, not something to work around in code.

### Capture the environment at the same time

```bash
lsb_release -d                 # which Ubuntu
uname -r                       # kernel (DKMS rebuilds on upgrades)
vainfo                         # VA-API: hardware H.264 decode, or software only?
lsusb                          # plug the phone in: does it enumerate, and as what?
```

Paste all of it into the session. It decides the decoder phase and confirms the USB IDs.

---

## 6. Machine setup

**Clone fresh. Do not build from the Windows checkout over an NTFS mount.** That checkout has
`core.autocrlf=true` and is physically CRLF; on Linux shell scripts fail with
`bad interpreter: /bin/bash^M`, and every file churns between the two systems. `.gitattributes` has
`* text=auto`, so a fresh clone gets correct LF.

```bash
git clone https://github.com/whoissaaif/mycam.git ~/mycam
cd ~/mycam
git checkout linux

# CLAUDE.md requires the no-reply address, and this machine has no identity yet
git config user.name  "Codewithaiyan"
git config user.email "201579830+Codewithaiyan@users.noreply.github.com"

sudo apt install build-essential cmake pkg-config \
     libusb-1.0-0-dev libavcodec-dev libavutil-dev libswscale-dev \
     libva-dev libssl-dev
```

---

## 7. Rules that will bite you

From `CLAUDE.md` — read it, but these are the ones that matter here:

- **`main` takes no direct commits.** Branch per change off current `main`, named `type/short-desc`
  (`feature/`, `fix/`, `docs/`, `refactor/`, `chore/`, `hotfix/`). PR, squash-merge, delete the branch.
  The `linux` branch is a holding area for this work; real changes should still go via PRs.
- **Conventional Commits**: `type(scope): summary`, imperative, under 72 characters.
- **`main` must always build.** A broken `main` is the top priority.
- **Commits carry the author from local `git config`** — never override on the command line, and always
  the GitHub no-reply address. The repo is public and an email cannot be taken back out.
- **Never commit** `inspo/`, `screenshots/`, `release/`, `keystore.properties`, `*.jks`, or any real
  email or secret.
- **`/docs/` is gitignored.** Internal planning notes are deliberately not published. A new file there
  needs `git add -f` to be tracked at all — decide deliberately whether it should be.
- **Protocol changes** touch `protocol/PROTOCOL.md`, `protocol/golden.txt`, `Protocol.kt` and
  `pc/companion/protocol.h` together, with a version bump. Phase 1 should need none of this.
- **Keep `SessionLoop` single-threaded** (`pc/companion/phone_link*.cpp`). A multi-threaded version
  deadlocked before.
- **Never log tokens, pairing codes or QR addresses.**
- **Track work in `TASKS.md` and `IMPROVEMENTS.md`** when something lands.

---

## 8. Facts about the codebase, verified on the Windows side

Checked by reading the code, not assumed. They save a day of discovery.

### Already portable — compiles on Linux unchanged

| File | What |
|---|---|
| `pc/companion/packet_parser.cpp` | MyCam packet parsing |
| `pc/companion/web_proto.cpp` | web protocol, tokens; unit-tested |
| `pc/vcam/frame_transform.h/.cpp` | **rotate / mirror / letterbox / scale on NV12** — explicitly "no Windows dependencies, so they can be unit-tested" |
| `pc/companion/ui_model.h`, `ui_layout.h`, `status_text.h`, `marquee.h` | UI element model; no Windows types at all (phase 4) |

### Windows-only, and Linux deletes rather than ports

| File | Why |
|---|---|
| `pc/companion/winusb_bind.cpp` | binds the WinUSB driver — Linux has no equivalent problem |
| `pc/companion/driver_task.cpp` | the SYSTEM scheduled task that avoids a UAC prompt — not needed |
| `pc/vcam/`, `pc/vcam-dshow/` | the two Windows virtual cameras |

### Numbers you will need

| | |
|---|---|
| AOA accessory product IDs | `0x2D00`–`0x2D05` — `IsAccessoryPid()` in `phone_link.cpp` |
| Phones worth switching | `kAndroidVendors[]` in `phone_link.cpp` — **24 vendor IDs** |
| Ports | TCP 47800 (Wi-Fi), UDP 47801 (discovery), TCP 47900 (iPhone page) |
| Frame format | NV12, tightly packed, max 3840×2160, plus rotation + mirror flags |
| Protocol | v3, big-endian, `MCAM` packets in, `MCMD` commands out |
| Tests | `mycam_tests` and `web_golden` via `ctest`; CI is `.github/workflows/ci.yml` |

---

## 9. Four corrections to `docs/LINUX-SUPPORT.md`

Found by checking its claims against the code. Worth raising with saif rather than silently working
around.

1. **The AOA handshake is in `phone_link.cpp`, not `driver_task.cpp`.** §2 and work-item 2 both name
   `driver_task.cpp`, which is only the Windows scheduled task — the one file Linux throws away.
   `kAoaSendString` / `kAoaStart` and the control transfers are in `phone_link.cpp`.

2. **Work-item 3 names the wrong module for rotation.** It says reuse `preview_convert`'s
   "scaling/rotation logic", but `preview_convert` only does NV12 → BGRA half-size downscale **for the
   settings-window preview**. The rotate/mirror/letterbox code is in **`pc/vcam/frame_transform.h`**,
   which is already portable and unit-tested. Use that.

3. **The udev rule is incomplete.** §2 matches `ATTR{idVendor}=="18d1"` only. That is the ID the phone
   reports *after* the AOA switch; the daemon must open it *before*, while it still reports the handset
   maker's ID. Without those, `libusb` returns `LIBUSB_ERROR_ACCESS` and the switch silently never
   happens. Cover the accessory range **and** the 24 vendors:

   ```
   # /etc/udev/rules.d/69-mycam.rules
   # After the switch (phone_link.cpp: IsAccessoryPid is 0x2D00-0x2D05)
   SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d0[0-5]", MODE="0660", TAG+="uaccess"
   # Before the switch — one line per entry of kAndroidVendors[]; generate it from that array
   SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", MODE="0660", TAG+="uaccess"   # Samsung
   SUBSYSTEM=="usb", ATTR{idVendor}=="2717", MODE="0660", TAG+="uaccess"   # Xiaomi
   SUBSYSTEM=="usb", ATTR{idVendor}=="0e8d", MODE="0660", TAG+="uaccess"   # MediaTek
   # … the rest of the 24
   ```

4. **The vendored libusb claim is correct** — `pc/third_party/libusb/libusb/os/` holds only
   `events_windows`, `threads_windows`, `windows_common`, `windows_usbdk`, `windows_winusb`. There is
   no POSIX backend. His advice stands: link the distro's `libusb-1.0` through `pkg-config` rather than
   restoring files into the vendored copy.

---

## 10. Things not to do

- **Do not vendor or statically link FFmpeg.** MyCam is MIT; `libavcodec` is LGPL or GPL depending on
  the build. Link dynamically against the distro package, the way the Windows build leans on the OS's
  own Media Foundation.
- **Do not restore the POSIX backend into `pc/third_party/libusb`.** One upstream-maintained Linux
  backend beats a second vendored copy to keep in sync.
- **Do not change anything Windows-visible** to make room for Linux. Windows 11 is shipping and
  Windows 10 just landed; both must keep building and behaving exactly as they do.
- **Do not add `#ifdef _WIN32` to shared logic.** Put the platform split behind an interface — a new
  file implementing an existing header (`decoder.h`, `app_settings.h`) rather than branches inside one.
- **Do not change the protocol** for phase 1. If it looks necessary, stop and ask: it means four
  codebases move together.
- **Do not ship `v4l2loopback`.** It is a third-party DKMS module; depend on the distro package and
  document the requirement, as OBS does.

---

## 11. Phase 1 is done when

From saif's doc, unchanged:

- [ ] `mycam-companion --daemon` builds and runs on the chosen test distro
- [ ] An Android phone over USB streams into a `v4l2loopback` device
- [ ] Verified in at least Zoom or OBS, **and** one Chromium-based browser, on the real machine
- [ ] Packaged as a `.deb` with the udev rule, installable with no manual `modprobe` or permission steps
      beyond the package's postinst
- [ ] `TASKS.md` and `IMPROVEMENTS.md` updated
- [ ] `protocol/golden.txt` vectors pass against the Linux build, and run in CI

---

## 12. First message back

After §5 and §6, report:

1. The four app checks — which listed MyCam, which showed the pattern
2. `lsb_release -d`, `uname -r`, `vainfo`, `lsusb` with the phone attached
3. Whether `docs/LINUX-SUPPORT.md` was in the clone
4. Whether saif has answered the §5 GUI question

Then the first real task is the build seam: a Linux target in `pc/CMakeLists.txt` that compiles the
portable modules listed in §8 and runs `mycam_tests` on Linux. Green tests there prove the protocol
survived the move, and that is the foundation everything else sits on.
