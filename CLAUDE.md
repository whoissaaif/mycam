# CLAUDE.md

Rules for this repo (MyCam: Android/iPhone → Windows 11 webcam). For background read TASKS.md,
IMPROVEMENTS.md and docs/ARCHITECTURE.md — this file is rules only.

- **Repo is public.** Verify with `gh repo view --json visibility`, don't trust this line. Never commit
  `inspo/`, `screenshots/`, `release/`, `keystore.properties`, `*.jks`, or any real email/secret.
- **Commit author = whoever is working**, from local `git config` — never override it on the command line.
  Always a GitHub no-reply email, never a real one: it can't be undone once pushed.
- **Fetch before any destructive git rewrite** (force-push, history rewrite) on a shared branch, and
  compare against `origin` first. A stale force-push has already dropped a collaborator's commits once.
- **Push every commit.** Tagging `vX.Y.Z` and publishing release binaries needs an explicit ask; a normal
  commit doesn't. Never change the repo's visibility without asking either.
- **Protocol changes** touch `protocol/PROTOCOL.md`, `protocol/golden.txt`, `Protocol.kt` and
  `pc/companion/protocol.h` together, with a version bump — never one side alone.
- **Keep `SessionLoop` (`pc/companion/phone_link*.cpp`) single-threaded.** A multi-thread version deadlocks.
- **UsbDk only switches the phone into accessory mode**; WinUSB (via the SYSTEM scheduled task) streams.
  Don't widen what that task runs.
- **Test the release APK (R8 on) on a real phone before publishing.** R8 has broken launch before.
- **Never tune behavior to the test phone** — every option comes from what the device reports
  (`CameraStreamer.modeFor`).
- **`applicationId` `io.github.whoissaaif.mycam` never changes** — that creates a new app on phones.
- **Keep:** opening the phone's own camera app stops the MyCam stream.
- **Design is Windows XP, Luna Blue, only — no dark mode.** Art comes from `design/tools/*.ps1`, never
  hand-edited.
- **Never log tokens, pairing codes, or QR addresses.**
- **Track work in TASKS.md and IMPROVEMENTS.md**; update both when something lands.
- **PowerShell 5 mangles UTF-8** (`·`, `×`, `→`). Use the Edit tool or `sed`, not `Set-Content`.
