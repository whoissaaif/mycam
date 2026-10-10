# Source images

Large source images that the art scripts composite from. They are **not** shipped as they are: the scripts
in `design/tools/` downscale and recompress them into the sizes each app needs.

| File | Size | Use |
|---|---|---|
| `hills-photo.png` | 1536 × 1024 | The hills-and-sky background: phone splash and first run, the camera pictures, installer and store art |
| `camera-paused-reference.png` | 1536 × 1024 | The reference composition for the "Camera paused" picture (frosted card, crossed-out camcorder, caps title) |

## Licence

⚠️ **To be recorded before the next release.** The owner supplied both images on 2026-10-10 and stated they
are free to use with no copyright issue. The exact source and licence still need to be written down here,
because these images ship inside the APK and the installer, and the repo is MIT.

Write down for each image: where it came from, the licence name, and whether attribution is required. If
attribution is required, add it to `app/src/main/assets/licenses` and to the README's licence section.

Everything else in `design/assets/` is generated from scratch by the scripts in `design/tools/`.
