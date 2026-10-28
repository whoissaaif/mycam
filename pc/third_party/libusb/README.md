# libusb (vendored, trimmed to the Windows backend)

Vendored from [libusb](https://libusb.info) (LGPL-2.1, see `COPYING`), nano version `LIBUSB_NANO` 11953 in
`libusb/version_nano.h`.

MyCam is Windows-only (see the repo's `CLAUDE.md`) and links libusb statically, so this tree keeps only what
`pc/CMakeLists.txt`'s `usb` target actually compiles: `libusb/core.c`, `descriptor.c`, `hotplug.c`, `io.c`,
`strerror.c`, `sync.c`, the Windows backend under `libusb/os/` (`events_windows`, `threads_windows`,
`windows_common`, `windows_usbdk`, `windows_winusb`), their headers, and `msvc/config.h`.

Upstream also ships backends for Linux, macOS, the BSDs, Haiku and Emscripten/WebUSB, plus its own build
systems (autotools, Xcode, Android NDK, MSVC project files), examples and test suite — none of which this
build ever touches, so they were dropped to cut the vendored tree from 152 files down to this set. To pull
in a newer libusb release, re-vendor from a fresh checkout of the upstream repo and re-apply this same trim
(or just copy the files listed above), then re-apply the local patches below.

## Local patches (marked `MyCam patch` in the source)

`libusb/os/windows_winusb.c` carries two small changes for phones in Android accessory mode with USB
debugging on (issue #66):

- **Interface numbers of vendor-named composite children.** `set_composite_interface` read the interface
  number from `MI_xx` in the instance ID. Vendor composite bus drivers (Samsung's `dg_ssudbus`, which claims
  Google's `18d1:2d01` on any PC that has the Samsung USB driver) name children `...&ADB\...` or
  `...&MS_COMP_MTP&<model>\...`, so every such child became interface 0. `composite_mi()` now falls back to
  the `USB\VID_xxxx&PID_xxxx&MI_xx` hardware ID those drivers still publish, and `find_mi_part()` only
  accepts a real `&MI_xx` part (not `MI_` inside a model name).
- **One busy interface doesn't fail the whole open.** `winusbx_open` opened every WinUSB interface of a
  composite device and gave up with `LIBUSB_ERROR_ACCESS` if any was held by another program (WinUSB
  handles are exclusive; a running adb server holds the phone's adb interface). It now skips those
  (claiming one then fails with `LIBUSB_ERROR_NOT_FOUND`) and fails only if every interface was busy.
  It reads the Win32 error right after `CreateFile`, before logging (a log callback can change the last
  error), and logs one warning naming the busy and the opened interfaces
  (`composite device: interface(s) 1 in use by another program, opened: 0`).
