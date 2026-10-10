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
| 6 | CAMERA | [v3] 18 bytes: u8 quality (0 720p, 1 1080p, 2 4K), u8 fps (30/60/120), u16 zoom×100, u16 zoomMin×100, u16 zoomMax×100, i8 ev, i8 evMin, i8 evMax, u8 evStep×100, u8 flags, u16 width, u16 height, u8 actual fps. Flags: 0x01 torch available, 0x02 torch on, 0x04 focus locked, 0x08 60 fps available, 0x10 4K available, 0x20 autofocus, 0x40 120 fps available (1.3.2; older PCs ignore it). Since 1.3.2 three more bytes follow (21 total): one frame-rate mask per quality (720p, 1080p, 4K) with 0x01 = 30, 0x02 = 60, 0x04 = 120 fps, worked out by the phone for every camera facing the current way (normal and high-speed modes, and the encoder at that exact size). PCs offer only the rates in the mask for the selected quality; with an 18-byte payload they fall back to the 0x08/0x40 flags. Width 0 = capabilities not known yet (camera not started since connecting). Sent after HELLO and whenever a setting changes. |

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
| 9 | SET_FPS | [v3] arg 30, 60 or 120 (120 since 1.3.2; restarts the camera if running). The phone falls back 120 → 60 → 30 to what the camera can do and reports the result as CAMERA actual fps. |
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

## Wireless transport (1.4, beta)

The same packets and commands can run over TCP instead of the USB accessory. The phone is the server and
the PC always connects, so Windows never needs an inbound firewall rule.

**Discovery (UDP, port 47801).** While "Find phones on Wi-Fi" is on, the PC broadcasts every 2 s, to the
directed broadcast address of each of its IPv4 networks and to 255.255.255.255:

    PC -> broadcast:   MYCAM?1 <pc name>            (ASCII/UTF-8, no terminator)
    phone -> PC:       MYCAM!1 <tcp port> <phone name>

The phone answers (unicast, to the sender's address and port) only while "Use over Wi-Fi" is on. It
remembers the PC name per IP address to show it when that PC connects. A phone that hasn't answered for 6 s
is forgotten.

"Scan for phones" on the PC sends the same probe (no new messages): one right away, then every 2 s for 6 s,
also while "Find phones on Wi-Fi" is off. It only lists the answers; with the search off, the PC connects
only when the user picks a phone.

**Session (TCP, port 47800).** The PC connects and runs the handshake below. After it, both directions
carry the usual packets and commands (the same bytes as over USB) inside encrypted records, and the session
continues exactly as over USB (`HELLO` every 2 s until answered, and so on).

One PC at a time. A USB connection always wins: the phone closes a Wi-Fi session when a cable link opens,
and the PC ends a Wi-Fi session when a phone is plugged in so the cable can take over.

**Retries.** The phone refused, or nobody answered within 60 s: the PC waits 2 minutes before trying that
phone again. A working session dropped: 2 s. The phone couldn't be reached: 10 s.

## Wireless security (1.4)

Every Wi-Fi session is authenticated and encrypted. A new PC must be **paired** once: both screens show
the same 6-digit code, and the user allows it on the phone. A paired PC connects without asking.

Primitives: P-256 ECDH (public keys as `04 | X | Y`, the shared secret is the X coordinate, big-endian),
SHA-256, HMAC-SHA256, HKDF-SHA256 (RFC 5869), AES-256-GCM with a 16-byte tag. `protocol/golden.txt` has
vectors for all of them (`wifi.*`, generated with the JDK), checked by both test suites.

**Handshake messages** (before encryption): `u16 length (type + body) | u8 type | body`, big-endian.

| Type | Name | Direction | Body |
|---|---|---|---|
| 1 | CLIENT_HELLO | PC → phone | `"MCHS"`, u8 version (1), u8 flags (bit 0: force pairing), PC id (16), ephemeral public key (65), Npc (16), u8 name length, PC name (UTF-8) |
| 2 | SERVER_HELLO | phone → PC | phone id (16), ephemeral public key (65), Nph (16), u8 mode (0 paired, 1 pairing), [pairing: commitment (32)], u8 name length, phone name |
| 3 | NONCE_A | PC → phone | Na (16), pairing only |
| 4 | NONCE_B | phone → PC | Nb (16), pairing only |
| 5 | PC_FINISHED | PC → phone | HMAC (32) |
| 6 | PHONE_FINISHED | phone → PC | HMAC (32) |
| 7 | REJECT | either | u8 reason (1 refused or timed out, 2 proof failed, 3 busy) |

Ids are random 16-byte values each side makes once. `z` = ECDH of the two ephemeral keys. The transcript `H`
is SHA-256 of messages 1 to 4 exactly as sent (length, type and body).

**Paired** (the phone knows a pairing key `K` for this PC id, and the PC didn't set "force pairing"):
SERVER_HELLO has mode 0, then both sides derive the session keys and exchange FINISHED. If the PC doesn't
have `K` for that phone any more, it sends REJECT and reconnects with "force pairing". If the phone's proof
check fails (it re-paired or forgot this PC), the PC forgets its key and pairs again.

**Pairing** (numeric comparison, as in Bluetooth): the phone picks Nb and sends the commitment
`SHA-256("MyCam commit v1" | Nb | phonePub | pcPub)` in SERVER_HELLO, before it sees Na. The PC sends Na, the
phone reveals Nb, and the PC checks the commitment. Both sides show
`code = (first 4 bytes of SHA-256(pcPub | phonePub | Na | Nb), as u32) mod 1,000,000` (6 digits). The
commitment means someone in between can't choose keys that make the codes match. The PC sends PC_FINISHED,
the phone checks it and asks the user. Only after "Allow" does the phone store `K` and send PHONE_FINISHED.
The PC then stores `K` too: the phone in its private app storage, the PC protected with DPAPI under
`HKCU\Software\MyCam\PairedPhones`.

    K (pairing) = HKDF(ikm = z, salt = Na | Nb, info = "MyCam pair v1", 32)
    session     = HKDF(ikm = z | K, salt = Npc | Nph, info = "MyCam session v1", 96)
                = PC-to-phone key (32) | phone-to-PC key (32) | finished key kFin (32)
    PC_FINISHED    = HMAC(kFin, "PC" | H)
    PHONE_FINISHED = HMAC(kFin, "PH" | H)

Each session uses fresh ephemeral keys, so a leaked pairing key doesn't reveal earlier sessions' video.

**Records** (after both FINISHED messages): `u32 BE ciphertext length | AES-256-GCM ciphertext | tag`.
The nonce is `u32 direction (0 PC to phone, 1 phone to PC) | u64 counter`, big-endian. The counter starts at 0
in each direction and goes up by one per record. A record that fails its tag check (tampered, replayed,
reordered or the wrong key) ends the session. The phone sends one record per packet; the maximum record is 16 MiB.

**Forgetting:** "Forget paired PCs" on the phone and "Forget Wi-Fi phones" on the PC delete the keys; the
next connection pairs again with a new code.
