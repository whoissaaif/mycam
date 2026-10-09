# MyCam wire protocol (version 3)

The phone and the PC talk over the two bulk endpoints of an Android Open Accessory (AOA) connection.
All integers are **big-endian**. Implementations:

- Android: `app/src/main/java/io/github/whoissaaif/mycam/Protocol.kt`
- Windows: `pc/companion/protocol.h` and `pc/companion/packet_parser.cpp`

Byte-exact examples live in [`golden.txt`](golden.txt), and both test suites check against them.
**When you change the protocol, update both implementations and `golden.txt` together.**

## AOA identification

The PC sends these strings during the AOA handshake. The phone's `res/xml/accessory_filter.xml` must
match the manufacturer and model.

| Index | Field | Value |
|---|---|---|
| 0 | manufacturer | `MyCam` |
| 1 | model | `MyCam Webcam` |
| 2 | description | `Use this phone as a USB webcam` |
| 3 | version | `1` |
| 4 | URI | `https://github.com/whoissaaif/mycam`, shown when the app isn't installed |
| 5 | serial | `0001` |

## Phone → PC packets

20-byte header followed by `length` bytes of payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x4D43414D` (`MCAM`) |
| 4 | 1 | type |
| 5 | 1 | flags (bit 0: key frame) |
| 6 | 2 | reserved, 0 |
| 8 | 8 | presentation time, µs (frames only; otherwise 0) |
| 16 | 4 | payload length (max 8 MiB) |

| Type | Name | Payload |
|---|---|---|
| 0 | HELLO | u16 protocol version (reply to `CMD_HELLO`). 1 = original, 2 = adds pause, 3 = adds video quality and camera controls |
| 1 | CONFIG | u16 width, u16 height, u16 sensor orientation, u8 facing, then SPS/PPS in Annex-B (may be empty) |
| 2 | FRAME | one H.264 access unit, Annex-B. Key frames carry SPS/PPS on most phones. |
| 3 | ORIENT | u16 device rotation, degrees clockwise (0/90/180/270) |
| 4 | STATE | u8 state (0 idle, 1 streaming, 2 error, 3 paused [v2]), u8 facing |
| 5 | LOG | UTF-8 text; the PC writes it to `mycam.log` |
| 6 | CAMERA | [v3] 18 bytes: u8 quality (0 720p, 1 1080p, 2 4K), u8 fps (30/60), u16 zoom×100, u16 zoomMin×100, u16 zoomMax×100, i8 ev, i8 evMin, i8 evMax, u8 evStep×100, u8 flags, u16 width, u16 height, u8 actual fps. Flags: 0x01 torch available, 0x02 torch on, 0x04 focus locked, 0x08 60 fps available, 0x10 4K available, 0x20 autofocus. Width 0 = capabilities not known yet (camera not started since connecting). Sent after HELLO and whenever a setting changes. |

Facing: 0 = back, 1 = front.

A receiver treats a header as valid only if the magic matches, the reserved bytes are 0, the type is
known and the length is within limits. Otherwise it skips one byte and looks again. This resyncs after
garbage, and stops a stray `MCAM` inside video data from being mistaken for a header. New packet types
therefore need a protocol version bump.

## PC → phone commands

Fixed 8 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x4D434D44` (`MCMD`) |
| 4 | 1 | command |
| 5 | 1 | argument |
| 6 | 2 | reserved, 0 |

| Cmd | Name | Meaning |
|---|---|---|
| 1 | HELLO | Phone replies with HELLO, STATE and ORIENT |
| 2 | START | Turn the camera on and stream (resends CONFIG and a key frame if already streaming) |
| 3 | STOP | Turn the camera off |
| 4 | KEYFRAME | Resend CONFIG and request a key frame |
| 5 | SET_FACING | Switch camera; argument = facing |
| 6 | PAUSE | [v2] Pause: camera off until RESUME. The phone stores this, so it survives reconnects. |
| 7 | RESUME | [v2] Resume; the camera turns back on if the PC still wants video |
| 8 | SET_QUALITY | [v3] arg 0 720p, 1 1080p, 2 4K (restarts the camera if running) |
| 9 | SET_FPS | [v3] arg 30 or 60 (restarts the camera if running) |
| 10 | SET_ZOOM | [v3] arg zoom×10, e.g. 6 = 0.6× ultrawide, 20 = 2× (applied live) |
| 11 | SET_EXPOSURE | [v3] arg signed int8 EV steps (applied live) |
| 12 | SET_TORCH | [v3] arg 0 off, 1 on (applied live; resets to off on reconnect) |
| 13 | SET_FOCUS | [v3] arg 0 continuous auto focus, 1 focus once and lock (applied live) |

Commands are validated the same way: magic, a known command number, and zero reserved bytes.

## Flow

1. PC opens the accessory and sends `HELLO` (repeated every 2 s until answered).
2. The phone answers `HELLO`, `STATE`, `ORIENT`.
3. When a PC app opens the MyCam camera, the PC sends `START`. The phone sends `CONFIG`, `STATE(streaming)`,
   then `FRAME`s.
4. About 4 s after the last PC app closes the camera, the PC sends `STOP`.

## Pause (v2)

The phone owns the pause state. It can be paused from the phone UI or by the PC (`PAUSE`), and resumed
either way (`RESUME`). While paused it reports `STATE(paused)`, keeps its camera off, and answers `START` with
`STATE(paused)` instead of video. The PC shows a "Camera paused" picture on the virtual camera.

The PC's automatic pause while Windows is locked is PC-only: the PC simply stops asking for video
(`STOP`) and never sends `START` until unlock. It does not change the phone's pause state.
