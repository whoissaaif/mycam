# Web camera: WebSocket message format

*For Aiyan, from saif. Reply to your Test 2 results. Status: proposal, open for your comments.*

## Your test results

Great results. They answer both unknowns from `iphone-web-plan.md`:
- **The certificate:** `wss://` works with our self-made certificate after one tap-through, so nobody has
  to install a certificate.
- **The hardware encoder:** WebCodecs H.264 is fast enough. 1080p30 at 30 fps (7.6 Mbps), 1080p60 at 59 fps
  (11.3 Mbps), no skipped frames, ~5 ms round trip.

The web-page approach is viable on an iPhone 13 (iOS 27).

## Proposal: reuse the MyCam protocol, one packet per WebSocket message

The protocol the Android app already uses over USB and Wi-Fi has everything on your list: timestamp,
keyframe flag, frame size, rotation, SPS/PPS, plus state, pause and the PC's controls. If the page speaks it,
the PC reads it with the code it already has. Decoding, the virtual camera, pause, the status pictures and
the PC's controls then work with no new PC logic, and there's only one format to maintain.

Rules:
- **Binary messages only.** Each WebSocket message carries exactly **one** complete packet or command.
- **Big-endian** for every number (JavaScript's `DataView` default).
- **Your sequence number goes.** WebSocket is ordered, and lost or late frames show up as gaps in the
  timestamps.
- **No extra encryption** inside the WebSocket: `wss://` (TLS) already encrypts everything. The Android
  Wi-Fi link has its own AES layer only because it runs over plain TCP.

Full spec: `protocol/PROTOCOL.md`, sections "Phone → PC packets", "PC → phone commands" and "Flow". Exact
example bytes: `protocol/golden.txt`. Please make your JavaScript produce exactly those bytes.

## Page → PC: packets

Every packet starts with this 20-byte header, then `length` bytes of payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `4D 43 41 4D` (`"MCAM"`) |
| 4 | 1 | type |
| 5 | 1 | flags: bit 0 = keyframe (FRAME only) |
| 6 | 2 | reserved, must be 0 |
| 8 | 8 | timestamp, µs, signed 64-bit (FRAME only, else 0) |
| 16 | 4 | payload length (max 8 MiB) |

| Type | Name | Payload | When the page sends it |
|---|---|---|---|
| 0 | HELLO | u16 protocol version = **3** | In reply to the PC's HELLO command |
| 1 | CONFIG | u16 width, u16 height, u16 sensor orientation, u8 facing (0 back, 1 front), then SPS/PPS in Annex-B | Before the first frame, and again whenever size or camera changes |
| 2 | FRAME | one encoded frame (access unit), Annex-B; flags bit 0 for keyframes | Every encoded frame |
| 3 | ORIENT | u16 rotation, degrees clockwise (0/90/180/270) | After HELLO, and whenever the phone is turned |
| 4 | STATE | u8 state (0 idle, 1 streaming, 2 error, 3 paused), u8 facing | After HELLO and whenever it changes |
| 5 | LOG | UTF-8 text, written to the PC's `mycam.log` | Any time: very useful for debugging |
| 6 | CAMERA | camera settings and abilities (see PROTOCOL.md) | Optional, can come later |

## PC → page: commands

Always 8 bytes: magic `4D 43 4D 44` (`"MCMD"`), u8 command, u8 argument, 2 reserved bytes (0).

| Cmd | Name | What the page does |
|---|---|---|
| 1 | HELLO | Reply HELLO, then STATE and ORIENT |
| 2 | START | Start the camera and encoder; send CONFIG, STATE(streaming), then FRAMEs. If already streaming: resend CONFIG and make the next frame a keyframe |
| 3 | STOP | Stop the camera (release the track), send STATE(idle) |
| 4 | KEYFRAME | Resend CONFIG; encode the next frame with `{ keyFrame: true }` |
| 5 | SET_FACING | arg 0 back / 1 front: switch `facingMode`, then resend CONFIG |
| 6 / 7 | PAUSE / RESUME | Camera off / back on (if the PC still wants video); report STATE(paused) |
| 8 | SET_QUALITY | arg 0 = 720p, 1 = 1080p, 2 = 4K: `applyConstraints` + reconfigure the encoder |
| 9 | SET_FPS | arg 30 / 60 / 120: same; fall back to what the camera allows |
| 10–13 | ZOOM / EXPOSURE / TORCH / FOCUS | Optional for now: do what the browser allows, ignore the rest |

The PC does the asking: it sends HELLO every 2 s until the page answers, START when an app on the PC opens
the camera, and STOP about 4 s after the last app closes it. So the camera is only on while something on the
PC actually uses it.

## Details for the page

- **Encoder setup:** `codec: "avc1.640028"` (or whatever level your test used), `avc: { format: "annexb" }`,
  `latencyMode: "realtime"`, `hardwareAcceleration: "prefer-hardware"`. Annex-B is what our decoder expects.
- **SPS/PPS:** with Annex-B output, keyframes carry them inline. Copy the SPS and PPS NAL units (types 7 and
  8) from the first keyframe into CONFIG. An empty SPS/PPS also works, because the PC can start from the
  keyframe alone, but a filled CONFIG is cleaner.
- **Timestamp:** use `EncodedVideoChunk.timestamp`. It's already in microseconds, so it goes straight into
  header bytes 8–15 (write it with `setBigInt64`).
- **Rotation, the one thing to test:**
  - The PC turns the picture by (sensor orientation + ORIENT).
  - If Safari already delivers upright frames, send sensor orientation **0** in CONFIG and keep ORIENT at 0.
  - If the frames stay in the camera's own orientation when you turn the phone, send the turn as ORIENT
    (from `screen.orientation.angle`).
  - Please check both portrait and landscape and tell me which it is.
- **Back-pressure:** if `ws.bufferedAmount` keeps growing, the link is slower than the encoder. Then skip
  frames until the next keyframe and lower the bitrate, as the Android app does, instead of queueing seconds
  of video.
- **Pairing:** the QR code's one-time secret goes in the address: `wss://<pc>:<port>/ws?t=<token>`. The PC
  refuses the upgrade without a valid token.

## Example: what the first seconds look like

    PC   -> page   MCMD 01 00 0000                  HELLO
    page -> PC     MCAM 00 ... 0003                 HELLO, version 3
    page -> PC     MCAM 04 ... 00 00                STATE idle, back camera
    page -> PC     MCAM 03 ... 0000                 ORIENT 0
    PC   -> page   MCMD 02 00 0000                  START (an app opened the camera)
    page -> PC     MCAM 01 ... 0780 0438 0000 00 <SPS/PPS>    CONFIG 1920x1080
    page -> PC     MCAM 04 ... 01 00                STATE streaming
    page -> PC     MCAM 02 01 ... <keyframe>        FRAME, keyframe
    page -> PC     MCAM 02 00 ... <frame>           FRAME ...

## Who does what (suggestion)

- **You (the page):**
  - the packets and commands above;
  - checking them against `golden.txt`;
  - the rotation test;
  - the back-pressure handling.
- **Me (the PC):**
  - the HTTPS/WebSocket server inside the companion, plugged into the existing single-threaded session loop
    (please don't add threads there: the earlier two-thread version could deadlock);
  - the certificate;
  - QR code and token pairing;
  - the firewall rule in the installer.

## Next test

Send real CONFIG and FRAME packets from the page. On the PC we feed them into the existing decoder and
measure the full delay, from the camera to the picture in an app, using the log lines we already have.

Open question for you: is anything missing for the page? For example, should the page report which
resolutions and frame rates the camera allows (the CAMERA packet), so the PC only offers those?
