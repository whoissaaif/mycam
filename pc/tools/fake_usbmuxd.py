#!/usr/bin/env python3
"""fake_usbmuxd.py - test MyCam's iPhone (usbmuxd) transport without an iPhone.

This script plays two roles at once:

  1. Apple's usbmuxd service: listens on 127.0.0.1:27015 (the port Apple Mobile
     Device Service uses on Windows) and speaks the plist-over-TCP protocol
     (Listen / Attached / Detached / Connect / Result).
  2. A fake iPhone: after a successful Connect the same socket becomes a raw
     tunnel, and this script answers the MyCam wire protocol (see
     protocol/PROTOCOL.md) exactly like the Android app would: HELLO, CONFIG,
     STATE, ORIENT, CAMERA, FRAME packets in reply to HELLO/START/STOP/... commands.

Run it, then start MyCamCompanion.exe and open the Windows Camera app.

Usage
    python pc/tools/fake_usbmuxd.py                      # normal run
    python pc/tools/fake_usbmuxd.py --selftest           # no companion needed
    python pc/tools/fake_usbmuxd.py --detach-after 10 --reattach-after 5
    python pc/tools/fake_usbmuxd.py --refuse-connect-count 3
    python pc/tools/fake_usbmuxd.py --attach-delay 5
    python pc/tools/fake_usbmuxd.py --synthetic          # no ffmpeg: structurally valid
                                                         # but NOT decodable video

Sample video (pc/tools/assets/sample.h264) is generated on first run with ffmpeg:

    ffmpeg -y -f lavfi -i testsrc=size=1280x720:rate=30 -t 10 -an -c:v libx264
           -profile:v baseline -pix_fmt yuv420p -bf 0 -g 30
           -x264-params keyint=30:min-keyint=30:scenecut=0:repeat-headers=1
           -f h264 pc/tools/assets/sample.h264

Python 3.8+, standard library only. ASCII-only output on purpose (Windows consoles).
"""

from __future__ import annotations

import argparse
import errno
import os
import plistlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from collections import namedtuple
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
DEFAULT_SAMPLE = SCRIPT_DIR / "assets" / "sample.h264"
DEFAULT_GOLDEN = REPO_ROOT / "protocol" / "golden.txt"

DEFAULT_MUX_PORT = 27015   # where Apple Mobile Device Service listens on Windows
DEFAULT_DEVICE_PORT = 5000  # the port the (future) iOS app listens on, see usbmux_transport
SYNTHETIC_FPS = 30.0

# --------------------------------------------------------------------------------------
# Logging
# --------------------------------------------------------------------------------------

_log_lock = threading.Lock()
LOG_ENABLED = True


def log(msg: str) -> None:
    if not LOG_ENABLED:
        return
    now = time.time()
    stamp = time.strftime("%H:%M:%S", time.localtime(now)) + ".%03d" % int((now % 1) * 1000)
    with _log_lock:
        print("%s %s" % (stamp, msg), flush=True)


# --------------------------------------------------------------------------------------
# MyCam wire protocol (mirrors protocol/PROTOCOL.md, protocol.h, Protocol.kt)
# --------------------------------------------------------------------------------------

PACKET_MAGIC = b"MCAM"
COMMAND_MAGIC = b"MCMD"
HEADER_SIZE = 20
COMMAND_SIZE = 8
MAX_PAYLOAD = 8 * 1024 * 1024
PROTOCOL_VERSION = 3

T_HELLO, T_CONFIG, T_FRAME, T_ORIENT, T_STATE, T_LOG, T_CAMERA = range(7)
TYPE_NAMES = {T_HELLO: "HELLO", T_CONFIG: "CONFIG", T_FRAME: "FRAME", T_ORIENT: "ORIENT",
              T_STATE: "STATE", T_LOG: "LOG", T_CAMERA: "CAMERA"}
FLAG_KEYFRAME = 0x01

ST_IDLE, ST_STREAMING, ST_ERROR, ST_PAUSED = 0, 1, 2, 3
STATE_NAMES = {ST_IDLE: "idle", ST_STREAMING: "streaming", ST_ERROR: "error", ST_PAUSED: "paused"}

(CMD_HELLO, CMD_START, CMD_STOP, CMD_KEYFRAME, CMD_SET_FACING, CMD_PAUSE, CMD_RESUME,
 CMD_SET_QUALITY, CMD_SET_FPS, CMD_SET_ZOOM, CMD_SET_EXPOSURE, CMD_SET_TORCH, CMD_SET_FOCUS) = range(1, 14)
CMD_NAMES = {1: "HELLO", 2: "START", 3: "STOP", 4: "KEYFRAME", 5: "SET_FACING", 6: "PAUSE",
             7: "RESUME", 8: "SET_QUALITY", 9: "SET_FPS", 10: "SET_ZOOM", 11: "SET_EXPOSURE",
             12: "SET_TORCH", 13: "SET_FOCUS"}

CAM_TORCH_AVAILABLE, CAM_TORCH_ON, CAM_FOCUS_LOCKED = 0x01, 0x02, 0x04
CAM_HAS_60FPS, CAM_HAS_4K, CAM_HAS_AUTOFOCUS = 0x08, 0x10, 0x20

Packet = namedtuple("Packet", "type flags pts payload")
_CAMERA_FMT = ">BBHHHbbbBBHHB"  # 18 bytes, see PROTOCOL.md TYPE_CAMERA


def build_packet(ptype: int, flags: int, pts_us: int, payload: bytes) -> bytes:
    return struct.pack(">4sBBHqI", PACKET_MAGIC, ptype, flags, 0, pts_us, len(payload)) + payload


def build_command(cmd: int, arg: int = 0) -> bytes:
    return struct.pack(">4sBBH", COMMAND_MAGIC, cmd, arg & 0xFF, 0)


def build_hello(version: int = PROTOCOL_VERSION) -> bytes:
    return struct.pack(">H", version)


def build_config(width: int, height: int, sensor_orientation: int, facing: int, sps_pps: bytes = b"") -> bytes:
    return struct.pack(">HHHB", width, height, sensor_orientation, facing) + sps_pps


def build_orient(degrees: int) -> bytes:
    return struct.pack(">H", degrees)


def build_state(state: int, facing: int) -> bytes:
    return bytes([state, facing])


def build_camera(quality, fps, zoom, zoom_min, zoom_max, ev, ev_min, ev_max, ev_step,
                 flags, width, height, actual_fps) -> bytes:
    return struct.pack(_CAMERA_FMT, quality, fps, zoom, zoom_min, zoom_max, ev, ev_min, ev_max,
                       ev_step, flags, width, height, actual_fps)


def parse_camera(payload: bytes) -> dict:
    keys = ("quality", "fps", "zoom", "zoom_min", "zoom_max", "ev", "ev_min", "ev_max", "ev_step",
            "flags", "width", "height", "actual_fps")
    return dict(zip(keys, struct.unpack(_CAMERA_FMT, payload[:18])))


class PacketParser:
    """Phone->PC packet splitter (used by the self-test client). Resyncs byte by byte like the PC."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf += data
        out = []
        while len(self.buf) >= HEADER_SIZE:
            magic, ptype, flags, reserved, pts, length = struct.unpack_from(">4sBBHqI", self.buf)
            if magic != PACKET_MAGIC or reserved != 0 or ptype > T_CAMERA or length > MAX_PAYLOAD:
                del self.buf[0]
                continue
            if len(self.buf) < HEADER_SIZE + length:
                break
            out.append(Packet(ptype, flags, pts, bytes(self.buf[HEADER_SIZE:HEADER_SIZE + length])))
            del self.buf[:HEADER_SIZE + length]
        return out


class CommandParser:
    """PC->phone command splitter. Same validation as Protocol.kt CommandParser."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf += data
        out = []
        pos = 0
        while len(self.buf) - pos >= COMMAND_SIZE:
            magic, cmd, arg, reserved = struct.unpack_from(">4sBBH", self.buf, pos)
            if magic == COMMAND_MAGIC and CMD_HELLO <= cmd <= CMD_SET_FOCUS and reserved == 0:
                out.append((cmd, arg))
                pos += COMMAND_SIZE
            else:
                pos += 1
        del self.buf[:pos]
        return out


def describe_packet(ptype: int, flags: int, pts: int, payload: bytes) -> str:
    try:
        if ptype == T_HELLO:
            return "version=%d" % struct.unpack(">H", payload[:2])[0]
        if ptype == T_CONFIG:
            w, h, o, f = struct.unpack(">HHHB", payload[:7])
            return "%dx%d sensor=%d facing=%d sps_pps=%dB" % (w, h, o, f, len(payload) - 7)
        if ptype == T_FRAME:
            return "%dB%s pts=%dus" % (len(payload), " KEY" if flags & FLAG_KEYFRAME else "", pts)
        if ptype == T_ORIENT:
            return "rotation=%d" % struct.unpack(">H", payload[:2])[0]
        if ptype == T_STATE:
            return "state=%s facing=%s" % (STATE_NAMES.get(payload[0], payload[0]),
                                           payload[1] if len(payload) > 1 else "-")
        if ptype == T_CAMERA:
            c = parse_camera(payload)
            return "%dx%d@%d zoom=%d ev=%d flags=0x%02X" % (c["width"], c["height"], c["actual_fps"],
                                                           c["zoom"], c["ev"], c["flags"])
        if ptype == T_LOG:
            return repr(payload.decode("utf-8", "replace"))
    except Exception:
        pass
    return "%dB" % len(payload)


# --------------------------------------------------------------------------------------
# H.264 Annex-B sample handling
# --------------------------------------------------------------------------------------

SC4 = b"\x00\x00\x00\x01"
_HIGH_PROFILES = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135}


class BitReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.pos >> 3
            if byte >= len(self.data):
                raise ValueError("out of bits")
            v = (v << 1) | ((self.data[byte] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def ue(self) -> int:
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            if zeros > 32:
                raise ValueError("bad exp-golomb")
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


class BitWriter:
    def __init__(self) -> None:
        self.bits = []

    def u(self, n: int, v: int) -> None:
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v: int) -> None:
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def to_bytes(self) -> bytes:
        bits = self.bits + [1]  # rbsp_stop_one_bit
        while len(bits) % 8:
            bits.append(0)
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def rbsp_from_nal_payload(payload: bytes) -> bytes:
    """Remove emulation-prevention bytes (00 00 03 -> 00 00)."""
    out = bytearray()
    zeros = 0
    for b in payload:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def escape_rbsp(data: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    for b in data:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def _skip_scaling_list(r: BitReader, size: int) -> None:
    last = nxt = 8
    for _ in range(size):
        if nxt != 0:
            nxt = (last + r.se() + 256) % 256
        if nxt != 0:
            last = nxt


def parse_sps_dimensions(nal: bytes):
    """Return (width, height) in pixels from an SPS NAL (including its 1-byte header)."""
    r = BitReader(rbsp_from_nal_payload(nal[1:]))
    profile = r.u(8)
    r.u(8)
    r.u(8)
    r.ue()
    chroma = 1
    if profile in _HIGH_PROFILES:
        chroma = r.ue()
        if chroma == 3:
            r.u(1)
        r.ue()
        r.ue()
        r.u(1)
        if r.u(1):
            for i in range(8 if chroma != 3 else 12):
                if r.u(1):
                    _skip_scaling_list(r, 16 if i < 6 else 64)
    r.ue()  # log2_max_frame_num_minus4
    poc_type = r.ue()
    if poc_type == 0:
        r.ue()
    elif poc_type == 1:
        r.u(1)
        r.se()
        r.se()
        for _ in range(r.ue()):
            r.se()
    r.ue()  # max_num_ref_frames
    r.u(1)  # gaps_in_frame_num_allowed
    mbs_w = r.ue() + 1
    map_units_h = r.ue() + 1
    frame_mbs_only = r.u(1)
    if not frame_mbs_only:
        r.u(1)
    r.u(1)  # direct_8x8_inference
    left = right = top = bottom = 0
    if r.u(1):
        left, right, top, bottom = r.ue(), r.ue(), r.ue(), r.ue()
    crop_x, crop_y = {0: (1, 1), 1: (2, 2), 2: (2, 1), 3: (1, 1)}[chroma]
    crop_y *= 2 - frame_mbs_only
    width = mbs_w * 16 - crop_x * (left + right)
    height = (2 - frame_mbs_only) * map_units_h * 16 - crop_y * (top + bottom)
    return width, height


def make_sps_nal(width: int, height: int, profile: int = 66) -> bytes:
    """Build a valid SPS NAL (used for the synthetic sample and parser tests)."""
    w = BitWriter()
    w.u(8, profile)
    w.u(8, 0xC0 if profile == 66 else 0)
    w.u(8, 31)
    w.ue(0)
    if profile in _HIGH_PROFILES:
        w.ue(1)
        w.ue(0)
        w.ue(0)
        w.u(1, 0)
        w.u(1, 0)
    w.ue(0)  # log2_max_frame_num_minus4
    w.ue(2)  # pic_order_cnt_type
    w.ue(1)  # max_num_ref_frames
    w.u(1, 0)
    mbw, mbh = (width + 15) // 16, (height + 15) // 16
    w.ue(mbw - 1)
    w.ue(mbh - 1)
    w.u(1, 1)  # frame_mbs_only
    w.u(1, 1)  # direct_8x8_inference
    crop_r, crop_b = (mbw * 16 - width) // 2, (mbh * 16 - height) // 2
    if crop_r or crop_b:
        w.u(1, 1)
        w.ue(0)
        w.ue(crop_r)
        w.ue(0)
        w.ue(crop_b)
    else:
        w.u(1, 0)
    w.u(1, 0)  # no VUI
    return b"\x67" + escape_rbsp(w.to_bytes())


def make_synthetic_stream(width: int = 1280, height: int = 720, gops: int = 2, gop_len: int = 4) -> bytes:
    """Structurally valid Annex-B stream (SPS/PPS/IDR/P) whose slices are NOT decodable video."""
    sps = make_sps_nal(width, height)
    pps = b"\x68\xce\x3c\x80"

    def body(n: int, seed: int) -> bytes:  # never contains 0x00, so no start-code ambiguity
        return bytes(((i * 7 + seed) % 250) + 3 for i in range(n))

    out = bytearray()
    for g in range(gops):
        out += SC4 + sps + SC4 + pps + SC4 + b"\x65\x88" + body(180, g)
        for p in range(1, gop_len):
            out += SC4 + b"\x41\x9a" + body(60, g * 10 + p)
    return bytes(out)


def split_nals(data: bytes):
    starts = []
    pos = data.find(b"\x00\x00\x01")
    while pos != -1:
        starts.append(pos)
        pos = data.find(b"\x00\x00\x01", pos + 3)
    nals = []
    for k, s in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(data)
        nal = data[s + 3:end].rstrip(b"\x00")
        if nal:
            nals.append(nal)
    return nals


def _first_mb_in_slice(nal: bytes) -> int:
    try:
        return BitReader(rbsp_from_nal_payload(nal[1:8])).ue()
    except ValueError:
        return 0


class Sample:
    """An Annex-B H.264 stream split into access units (one VCL NAL per frame assumed, with
    first_mb_in_slice used to keep multi-slice pictures together)."""

    def __init__(self, data: bytes, fps: float, label: str) -> None:
        self.label = label
        self.fps = fps
        self.interval_us = int(1_000_000 / fps)
        nals = split_nals(data)
        if not nals:
            raise ValueError("no NAL units found")
        sps = next((n for n in nals if n[0] & 0x1F == 7), None)
        pps = next((n for n in nals if n[0] & 0x1F == 8), None)
        if sps is None or pps is None:
            raise ValueError("sample has no SPS/PPS")
        self.sps_pps = SC4 + sps + SC4 + pps
        try:
            self.width, self.height = parse_sps_dimensions(sps)
        except Exception as exc:  # noqa: BLE001
            log("warning: could not parse SPS (%s); assuming 1280x720" % exc)
            self.width, self.height = 1280, 720
        self.units = self._group(nals)
        if not self.units or not self.units[0][1]:
            raise ValueError("sample must start with an IDR (key) frame")

    @staticmethod
    def _group(nals):
        units, cur, cur_vcl = [], [], False

        def flush():
            nonlocal cur, cur_vcl
            if cur_vcl:
                units.append((b"".join(SC4 + n for n in cur), any(n[0] & 0x1F == 5 for n in cur)))
            cur, cur_vcl = [], False

        for nal in nals:
            t = nal[0] & 0x1F
            if t in (1, 5):
                if cur_vcl and _first_mb_in_slice(nal) == 0:
                    flush()
                cur.append(nal)
                cur_vcl = True
            else:
                if cur_vcl:
                    flush()
                cur.append(nal)
        flush()
        return units

    def describe(self) -> str:
        keys = sum(1 for _, k in self.units if k)
        return "%s: %dx%d, %d frames (%d key), %.0f fps" % (self.label, self.width, self.height,
                                                          len(self.units), keys, self.fps)


def _fmt_cmd(args) -> str:
    return " ".join(a if re.match(r"^[A-Za-z0-9_./\\:-]+$", a) else '"%s"' % a for a in args)


def ffmpeg_command(path: Path):
    return ["ffmpeg", "-y", "-f", "lavfi", "-i", "testsrc=size=1280x720:rate=30", "-t", "10", "-an",
            "-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p", "-bf", "0", "-g", "30",
            "-x264-params", "keyint=30:min-keyint=30:scenecut=0:repeat-headers=1",
            "-f", "h264", str(path)]


def load_sample(path: Path, synthetic: bool, fps: float) -> Sample:
    if synthetic:
        return Sample(make_synthetic_stream(), SYNTHETIC_FPS, "synthetic (NOT decodable video)")
    if not path.exists():
        cmd = ffmpeg_command(path)
        if shutil.which("ffmpeg") is None:
            print("Sample video %s is missing and ffmpeg is not installed." % path)
            print("Install it (winget install Gyan.FFmpeg), reopen the terminal, then run:")
            print("  " + _fmt_cmd(cmd))
            print("Or run with --synthetic to test connect/detach/retry without real video.")
            sys.exit(2)
        path.parent.mkdir(parents=True, exist_ok=True)
        print("Generating %s with ffmpeg ..." % path)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0 or not path.exists():
            print("ffmpeg failed:\n" + proc.stderr.decode("utf-8", "replace")[-1500:])
            sys.exit(2)
    return Sample(path.read_bytes(), fps, str(path))


# --------------------------------------------------------------------------------------
# usbmuxd protocol (16-byte little-endian header + XML plist)
# --------------------------------------------------------------------------------------

MUX_VERSION_PLIST = 1
MUX_TYPE_PLIST = 8
RESULT_OK, RESULT_BAD_COMMAND, RESULT_BAD_DEVICE, RESULT_REFUSED = 0, 1, 2, 3


def swap16(v: int) -> int:
    """usbmuxd's PortNumber is htons(port) stored in the plist integer."""
    return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF)


def mux_encode(tag: int, obj) -> bytes:
    payload = plistlib.dumps(obj, fmt=plistlib.FMT_XML)
    return struct.pack("<IIII", 16 + len(payload), MUX_VERSION_PLIST, MUX_TYPE_PLIST, tag) + payload


def recv_exact(sock: socket.socket, n: int):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            if not buf:
                return None
            raise ConnectionError("connection closed mid-message")
        buf += chunk
    return bytes(buf)


def mux_read(sock: socket.socket):
    """Return (tag, dict) or None on clean EOF."""
    hdr = recv_exact(sock, 16)
    if hdr is None:
        return None
    length, _version, _mtype, tag = struct.unpack("<IIII", hdr)
    if length < 16 or length > (1 << 20):
        raise ConnectionError("bad usbmuxd length %d" % length)
    body = recv_exact(sock, length - 16) if length > 16 else b""
    if body is None:
        raise ConnectionError("connection closed mid-message")
    return tag, (plistlib.loads(body) if body else {})


def result_msg(number: int) -> dict:
    return {"MessageType": "Result", "Number": number}


# --------------------------------------------------------------------------------------
# The fake iPhone (speaks the MyCam protocol over the tunnel)
# --------------------------------------------------------------------------------------

class FakePhone:
    """One tunnel = one phone session. All state changes and sends happen under self.lock, so a
    PAUSE/STOP can never be followed by a stray FRAME."""

    SENSOR_ORIENTATION = 90

    def __init__(self, sock: socket.socket, sample: Sample) -> None:
        self.sock = sock
        self.sample = sample
        self.lock = threading.RLock()
        self.closed = threading.Event()
        self.facing = 0           # 0 back, 1 front
        self.wanted = False       # PC asked for video
        self.paused = False
        self.streaming = False
        self.caps_known = False   # CAMERA width is 0 until the camera has started once
        self.stream_gen = 0
        self.pts_us = 1_000_000   # keeps increasing across sample loops, stops and restarts
        self.frames_sent = 0
        self.quality, self.fps = 1, 30
        self.zoom, self.zoom_min, self.zoom_max = 100, 60, 1000
        self.ev, self.ev_min, self.ev_max, self.ev_step = 0, -12, 12, 33
        self.torch = False
        self.focus_locked = False
        self.said_hello = False
        self.streamer = threading.Thread(target=self._stream_loop, daemon=True)

    # ---- sending -------------------------------------------------------------------

    def _send(self, ptype: int, payload: bytes, flags: int = 0, pts: int = 0) -> None:
        self.sock.sendall(build_packet(ptype, flags, pts, payload))
        if ptype == T_FRAME:
            self.frames_sent += 1
            if self.frames_sent % 30 == 1:
                log("phone -> FRAME %s (every 30th frame logged, %d sent)" %
                    (describe_packet(ptype, flags, pts, payload), self.frames_sent))
        else:
            log("phone -> %s %s" % (TYPE_NAMES[ptype], describe_packet(ptype, flags, pts, payload)))

    def _state_byte(self) -> int:
        return ST_PAUSED if self.paused else (ST_STREAMING if self.streaming else ST_IDLE)

    def _send_state(self, state=None) -> None:
        self._send(T_STATE, build_state(self._state_byte() if state is None else state, self.facing))

    def _send_config(self) -> None:
        self._send(T_CONFIG, build_config(self.sample.width, self.sample.height,
                                          self.SENSOR_ORIENTATION, self.facing, self.sample.sps_pps))

    def _send_camera(self) -> None:
        flags = CAM_TORCH_AVAILABLE | CAM_HAS_60FPS | CAM_HAS_4K | CAM_HAS_AUTOFOCUS
        if self.torch:
            flags |= CAM_TORCH_ON
        if self.focus_locked:
            flags |= CAM_FOCUS_LOCKED
        width, height, actual = (self.sample.width, self.sample.height, int(round(self.sample.fps))) \
            if self.caps_known else (0, 0, 0)
        self._send(T_CAMERA, build_camera(self.quality, self.fps, self.zoom, self.zoom_min, self.zoom_max,
                                          self.ev, self.ev_min, self.ev_max, self.ev_step, flags,
                                          width, height, actual))

    # ---- camera on/off -------------------------------------------------------------

    def _start_camera(self) -> None:
        """Protocol order: CONFIG, STATE(streaming), then FRAMEs (first one a key frame)."""
        self.streaming = False
        self.caps_known = True
        self._send_config()
        self._send_state(ST_STREAMING)
        self._send_camera()
        self.stream_gen += 1
        self.streaming = True

    # ---- command handling ----------------------------------------------------------

    def _handle_command(self, cmd: int, arg: int) -> None:
        log("PC -> phone %s arg=%d" % (CMD_NAMES.get(cmd, cmd), arg))
        with self.lock:
            if cmd == CMD_HELLO:
                self._send(T_HELLO, build_hello())
                self._send_state()
                self._send(T_ORIENT, build_orient(0))
                self._send_camera()
                if not self.said_hello:
                    self.said_hello = True
                    self._send(T_LOG, b"fake_usbmuxd: fake iPhone tunnel is up")
            elif cmd == CMD_START:
                self.wanted = True
                if self.paused:
                    self._send_state(ST_PAUSED)
                else:
                    self._start_camera()  # also covers "already streaming": resend CONFIG + key frame
            elif cmd == CMD_STOP:
                self.wanted = False
                self.streaming = False
                self._send_state()
            elif cmd == CMD_KEYFRAME:
                self._send_config()
                if self.streaming:
                    self.stream_gen += 1  # restart from the sample's first (key) frame
            elif cmd == CMD_SET_FACING:
                self.facing = 1 if arg == 1 else 0
                if self.streaming:
                    self._start_camera()
                else:
                    self._send_state()
                    self._send_camera()
            elif cmd == CMD_PAUSE:
                self.paused = True
                self.streaming = False
                self._send_state()
            elif cmd == CMD_RESUME:
                self.paused = False
                if self.wanted:
                    self._start_camera()
                else:
                    self._send_state()
            elif cmd in (CMD_SET_QUALITY, CMD_SET_FPS):
                if cmd == CMD_SET_QUALITY and arg in (0, 1, 2):
                    self.quality = arg
                elif cmd == CMD_SET_FPS and arg in (30, 60):
                    self.fps = arg
                # The fake always streams the sample as-is; only the reported setting changes.
                if self.streaming:
                    self._start_camera()  # real phones restart the camera here
                else:
                    self._send_camera()
            elif cmd == CMD_SET_ZOOM:
                self.zoom = max(self.zoom_min, min(self.zoom_max, arg * 10))
                self._send_camera()
            elif cmd == CMD_SET_EXPOSURE:
                ev = arg - 256 if arg > 127 else arg
                self.ev = max(self.ev_min, min(self.ev_max, ev))
                self._send_camera()
            elif cmd == CMD_SET_TORCH:
                self.torch = bool(arg)
                self._send_camera()
            elif cmd == CMD_SET_FOCUS:
                self.focus_locked = bool(arg)
                self._send_camera()

    # ---- frame pacing --------------------------------------------------------------

    def _stream_loop(self) -> None:
        sample = self.sample
        interval = 1.0 / sample.fps
        seen_gen = None
        idx = 0
        next_t = 0.0
        try:
            while not self.closed.is_set():
                with self.lock:
                    active, gen = self.streaming, self.stream_gen
                if not active:
                    seen_gen = None
                    self.closed.wait(0.01)
                    continue
                if gen != seen_gen:
                    seen_gen, idx, next_t = gen, 0, time.monotonic()
                delay = next_t - time.monotonic()
                if delay > 0:
                    self.closed.wait(delay)
                with self.lock:
                    if not self.streaming or self.stream_gen != gen or self.closed.is_set():
                        continue
                    au, is_key = sample.units[idx]
                    self._send(T_FRAME, au, FLAG_KEYFRAME if is_key else 0, self.pts_us)
                    self.pts_us += sample.interval_us
                idx = (idx + 1) % len(sample.units)
                next_t += interval
                if next_t < time.monotonic() - 0.25:
                    next_t = time.monotonic()
        except OSError:
            pass
        finally:
            self.close()

    # ---- lifecycle -----------------------------------------------------------------

    def run(self) -> None:
        self.streamer.start()
        parser = CommandParser()
        try:
            while True:
                data = self.sock.recv(4096)
                if not data:
                    break
                for cmd, arg in parser.feed(data):
                    self._handle_command(cmd, arg)
        except OSError:
            pass
        finally:
            self.close()
            self.streamer.join(timeout=1.0)
            log("tunnel closed")

    def close(self) -> None:
        self.closed.set()
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------------------
# The fake usbmuxd server
# --------------------------------------------------------------------------------------

class PortInUse(Exception):
    pass


class Listener:
    def __init__(self, conn: socket.socket) -> None:
        self.conn = conn
        self.lock = threading.Lock()

    def send(self, tag: int, obj) -> None:
        data = mux_encode(tag, obj)
        with self.lock:
            self.conn.sendall(data)


class Server:
    def __init__(self, host="127.0.0.1", port=DEFAULT_MUX_PORT, device_port=DEFAULT_DEVICE_PORT,
                 sample=None, detach_after=None, reattach_after=None, attach_delay=0.0,
                 refuse_connect_count=0, device_id=1, serial="FAKE0001") -> None:
        self.host, self.port, self.device_port = host, port, device_port
        self.sample = sample
        self.detach_after, self.reattach_after = detach_after, reattach_after
        self.attach_delay = attach_delay
        self.refuse_connect_count = refuse_connect_count
        self.device_id, self.serial = device_id, serial
        self.lock = threading.RLock()
        self.attached = attach_delay <= 0
        self.listeners = []
        self.tunnels = []
        self.refused = 0
        self.listen_seen = False
        self._detach_armed = False
        self.timers = []
        self.stopping = False
        self.lsock = None

    # ---- lifecycle -----------------------------------------------------------------

    def start(self) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if os.name != "nt":
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((self.host, self.port))
        except OSError as exc:
            s.close()
            if exc.errno in (errno.EADDRINUSE, 10048, 10013):
                raise PortInUse(str(exc))
            raise
        s.listen(16)
        s.settimeout(0.5)
        self.lsock = s
        self.port = s.getsockname()[1]
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def stop(self) -> None:
        self.stopping = True
        for t in self.timers:
            t.cancel()
        with self.lock:
            tunnels, listeners = list(self.tunnels), list(self.listeners)
        for t in tunnels:
            t.close()
        for l in listeners:
            try:
                l.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            l.conn.close()
        if self.lsock:
            self.lsock.close()

    def _accept_loop(self) -> None:
        while not self.stopping:
            try:
                conn, _addr = self.lsock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            conn.settimeout(None)
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _timer(self, seconds: float, fn) -> None:
        t = threading.Timer(seconds, fn)
        t.daemon = True
        self.timers.append(t)
        t.start()

    # ---- messages ------------------------------------------------------------------

    def attached_msg(self) -> dict:
        # Extra keys mirror what real usbmuxd sends; the companion's parser must ignore them.
        return {"MessageType": "Attached", "DeviceID": self.device_id,
                "Properties": {"ConnectionSpeed": 480000000, "ConnectionType": "USB",
                               "DeviceID": self.device_id, "LocationID": 0, "ProductID": 4776,
                               "SerialNumber": self.serial}}

    def _broadcast(self, msg: dict) -> None:
        dead = []
        for l in self.listeners:
            try:
                l.send(0, msg)
            except OSError:
                dead.append(l)
        for l in dead:
            self.listeners.remove(l)
        log("usbmuxd -> %s DeviceID=%s tag=0 (to %d listener(s))" %
            (msg["MessageType"], msg["DeviceID"], len(self.listeners)))

    def _attach(self) -> None:
        with self.lock:
            if self.attached:
                return
            self.attached = True
            self._broadcast(self.attached_msg())
        if self.attach_delay > 0 and not self.stopping:
            pass

    def detach(self) -> None:
        with self.lock:
            if not self.attached:
                return
            self.attached = False
            log("device unplugged (simulated)")
            self._broadcast({"MessageType": "Detached", "DeviceID": self.device_id})
            tunnels = list(self.tunnels)
            if self.reattach_after is not None and not self.stopping:
                log("will re-attach in %.1fs" % self.reattach_after)
                self._timer(self.reattach_after, self._attach)
        for t in tunnels:
            t.close()

    # ---- per-connection handling ---------------------------------------------------

    def _handle(self, conn: socket.socket) -> None:
        try:
            msg = mux_read(conn)
            if msg is None:
                return
            tag, req = msg
            mtype = req.get("MessageType") if isinstance(req, dict) else None
            if mtype == "Listen":
                self._do_listen(conn, tag, req)
            elif mtype == "Connect":
                self._do_connect(conn, tag, req)
            elif mtype == "ListDevices":
                self._do_list_devices(conn, tag)
            else:
                log("usbmuxd <- %r tag=%d (unsupported)" % (mtype, tag))
                conn.sendall(mux_encode(tag, result_msg(RESULT_BAD_COMMAND)))
        except (OSError, ConnectionError, plistlib.InvalidFileException, ValueError) as exc:
            log("connection error: %s" % exc)
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _do_listen(self, conn, tag, req) -> None:
        log("usbmuxd <- Listen tag=%d ClientVersionString=%r ProgName=%r" %
            (tag, req.get("ClientVersionString"), req.get("ProgName")))
        listener = Listener(conn)
        with self.lock:
            first = not self.listen_seen
            self.listen_seen = True
            self.listeners.append(listener)
            listener.send(tag, result_msg(RESULT_OK))
            log("usbmuxd -> Result Number=0 tag=%d" % tag)
            if self.attached:
                listener.send(0, self.attached_msg())
                log("usbmuxd -> Attached DeviceID=%d SerialNumber=%s tag=0" % (self.device_id, self.serial))
        if first and self.attach_delay > 0:
            log("device will attach in %.1fs (counted from the first Listen)" % self.attach_delay)
            self._timer(self.attach_delay, self._attach)
        try:
            while conn.recv(4096):  # a real client sends nothing more; wait for it to hang up
                pass
        except OSError:
            pass
        with self.lock:
            if listener in self.listeners:
                self.listeners.remove(listener)
        log("Listen client disconnected")

    def _do_list_devices(self, conn, tag) -> None:
        log("usbmuxd <- ListDevices tag=%d" % tag)
        with self.lock:
            devices = [self.attached_msg()] if self.attached else []
        conn.sendall(mux_encode(tag, {"DeviceList": devices}))
        try:
            while conn.recv(4096):
                pass
        except OSError:
            pass

    def _do_connect(self, conn, tag, req) -> None:
        dev, port = req.get("DeviceID"), req.get("PortNumber")
        log("usbmuxd <- Connect tag=%d DeviceID=%r PortNumber=%r (= port %s in host order)" %
            (tag, dev, port, swap16(port) if isinstance(port, int) else "?"))
        with self.lock:
            if dev != self.device_id or not self.attached:
                number, why = RESULT_BAD_DEVICE, "no such device"
            elif port != swap16(self.device_port):
                number = RESULT_REFUSED
                why = "nothing listens there (expected PortNumber=%d = htons(%d))" % (
                    swap16(self.device_port), self.device_port)
            elif self.refused < self.refuse_connect_count:
                self.refused += 1
                number, why = RESULT_REFUSED, "simulated refusal %d/%d" % (self.refused, self.refuse_connect_count)
            else:
                number, why = RESULT_OK, "ok"
        log("usbmuxd -> Result Number=%d tag=%d (%s)" % (number, tag, why))
        conn.sendall(mux_encode(tag, result_msg(number)))
        if number != RESULT_OK:
            return
        phone = FakePhone(conn, self.sample)
        with self.lock:
            self.tunnels.append(phone)
            if self.detach_after is not None and not self._detach_armed:
                self._detach_armed = True
                log("tunnel up; will simulate unplug in %.1fs (once)" % self.detach_after)
                self._timer(self.detach_after, self.detach)
            else:
                log("tunnel up")
        try:
            phone.run()
        finally:
            with self.lock:
                if phone in self.tunnels:
                    self.tunnels.remove(phone)


# --------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------

class Results:
    def __init__(self) -> None:
        self.passed = self.failed = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        if ok:
            self.passed += 1
            print("PASS  " + name)
        else:
            self.failed += 1
            print("FAIL  %s%s" % (name, (" -- " + detail) if detail else ""))
        return ok


def parse_golden(path: Path):
    entries = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, hexs = line.split("=", 1)
        entries[name.strip()] = bytes.fromhex(hexs.replace(" ", ""))
    return entries


def golden_expectations():
    P, C = build_packet, build_command
    return {
        "packet.hello": P(T_HELLO, 0, 0, build_hello(1)),
        "packet.config": P(T_CONFIG, 0, 0, build_config(1920, 1080, 90, 1, bytes.fromhex("0000000167"))),
        "packet.frame": P(T_FRAME, FLAG_KEYFRAME, 0x0102030405060708, bytes.fromhex("DEADBEEF")),
        "packet.orient": P(T_ORIENT, 0, 0, build_orient(270)),
        "packet.state": P(T_STATE, 0, 0, build_state(ST_STREAMING, 1)),
        "packet.state_paused": P(T_STATE, 0, 0, build_state(ST_PAUSED, 0)),
        "packet.log": P(T_LOG, 0, 0, b"hi"),
        "packet.camera": P(T_CAMERA, 0, 0, build_camera(1, 30, 100, 60, 1000, 0, -12, 12, 33, 0x19,
                                                        1920, 1080, 30)),
        "command.hello": C(CMD_HELLO), "command.start": C(CMD_START), "command.stop": C(CMD_STOP),
        "command.keyframe": C(CMD_KEYFRAME), "command.facing_front": C(CMD_SET_FACING, 1),
        "command.pause": C(CMD_PAUSE), "command.resume": C(CMD_RESUME),
        "command.quality_4k": C(CMD_SET_QUALITY, 2), "command.fps_60": C(CMD_SET_FPS, 60),
        "command.zoom_2x": C(CMD_SET_ZOOM, 20), "command.exposure_m2": C(CMD_SET_EXPOSURE, 0xFE),
        "command.torch_on": C(CMD_SET_TORCH, 1), "command.focus_lock": C(CMD_SET_FOCUS, 1),
    }


class MuxClient:
    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)

    def send(self, tag: int, obj) -> None:
        self.sock.sendall(mux_encode(tag, obj))

    def recv(self, timeout: float = 3.0):
        """(tag, dict), or None on timeout. Raises ConnectionError on EOF."""
        self.sock.settimeout(timeout)
        try:
            msg = mux_read(self.sock)
        except socket.timeout:
            return None
        if msg is None:
            raise ConnectionError("EOF")
        return msg

    def recv_until(self, pred, timeout: float = 3.0):
        end = time.monotonic() + timeout
        got = []
        while time.monotonic() < end:
            try:
                msg = self.recv(max(0.05, end - time.monotonic()))
            except ConnectionError:
                break
            if msg is None:
                break
            got.append(msg)
            if pred(msg):
                return msg, got
        return None, got

    def close(self) -> None:
        self.sock.close()


class TunnelClient:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.parser = PacketParser()
        self.closed = False

    def cmd(self, cmd: int, arg: int = 0) -> None:
        self.sock.sendall(build_command(cmd, arg))

    def pump(self, seconds: float, until=None):
        new = []
        end = time.monotonic() + seconds
        while not self.closed:
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            self.sock.settimeout(remaining)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                break
            except OSError:
                self.closed = True
                break
            if not data:
                self.closed = True
                break
            new.extend(self.parser.feed(data))
            if until and until(new):
                break
        return new


def _of(pkts, ptype):
    return [p for p in pkts if p.type == ptype]


def _first_idx(pkts, pred):
    for i, p in enumerate(pkts):
        if pred(p):
            return i
    return -1


def mux_connect(port: int, tag: int, dev: int, port_number: int):
    c = MuxClient(port)
    c.send(tag, {"MessageType": "Connect", "ClientVersionString": "selftest", "ProgName": "selftest",
                 "DeviceID": dev, "PortNumber": port_number})
    reply = c.recv(3.0)
    return c, reply


def result_of(reply):
    return reply[1].get("Number") if reply and reply[1].get("MessageType") == "Result" else None


def selftest(args) -> int:
    global LOG_ENABLED
    LOG_ENABLED = args.verbose
    r = Results()
    golden_path = Path(args.golden) if args.golden else DEFAULT_GOLDEN

    print("== 1. Wire protocol vs %s" % golden_path)
    expect = golden_expectations()
    try:
        golden = parse_golden(golden_path)
    except OSError as exc:
        golden = {}
        r.check("read golden.txt", False, str(exc))
    for name, want in expect.items():
        if golden:
            r.check("golden %s" % name, golden.get(name) == want,
                    "built %s, golden %s" % (want.hex(), golden.get(name, b"<missing>").hex()))
    for name in sorted(set(golden) - set(expect)):
        r.check("golden %s is known to this script" % name, False,
                "golden.txt has an entry this script does not build; update fake_usbmuxd.py")
    if golden:
        stream = b"\x01\x02garbage" + b"".join(v for k, v in golden.items() if k.startswith("packet."))
        pp, got = PacketParser(), []
        for i in range(len(stream)):  # byte-at-a-time = worst-case fragmentation
            got += pp.feed(stream[i:i + 1])
        r.check("PacketParser resyncs and parses all golden packets", len(got) == 8, "got %d" % len(got))
        cstream = b"junk" + b"".join(v for k, v in golden.items() if k.startswith("command."))
        cmds = CommandParser().feed(cstream)
        r.check("CommandParser parses all golden commands", len(cmds) == 13 and cmds[0] == (1, 0) and
                cmds[-1] == (13, 1), str(cmds))

    print("== 2. H.264 helpers")
    for w, h, prof in [(1280, 720, 66), (1920, 1080, 66), (640, 480, 66), (1920, 1080, 100), (3840, 2160, 100)]:
        try:
            got_wh = parse_sps_dimensions(make_sps_nal(w, h, prof))
        except Exception as exc:  # noqa: BLE001
            got_wh = str(exc)
        r.check("SPS parse %dx%d profile %d" % (w, h, prof), got_wh == (w, h), str(got_wh))
    real = DEFAULT_SAMPLE if not args.sample else Path(args.sample)
    if real.exists():
        try:
            s = Sample(real.read_bytes(), args.fps, str(real))
            r.check("real sample parses: " + s.describe(), len(s.units) > 1)
        except Exception as exc:  # noqa: BLE001
            r.check("real sample parses", False, str(exc))
    else:
        print("SKIP  real sample not present (%s)" % real)

    print("== 3. usbmuxd + fake iPhone over loopback")
    sample = Sample(make_synthetic_stream(), 60.0, "synthetic")
    srv = Server(port=0, sample=sample)
    srv.start()
    try:
        # Listen
        l1 = MuxClient(srv.port)
        l1.send(7, {"MessageType": "Listen", "ClientVersionString": "selftest", "ProgName": "selftest"})
        reply = l1.recv()
        r.check("Listen -> Result 0 with the request tag echoed", reply == (7, result_msg(0)), str(reply))
        att = l1.recv()
        props = att[1].get("Properties", {}) if att else {}
        r.check("Attached event (DeviceID 1, SerialNumber FAKE0001, USB, extra keys present)",
                bool(att) and att[1].get("MessageType") == "Attached" and att[1].get("DeviceID") == 1 and
                props.get("SerialNumber") == "FAKE0001" and props.get("ConnectionType") == "USB" and
                "ConnectionSpeed" in props and "ProductID" in props, str(att))
        # Connect rejections
        c, rep = mux_connect(srv.port, 9, 2, swap16(5000)); c.close()
        r.check("Connect to unknown device -> Result 2, tag echoed", result_of(rep) == 2 and rep[0] == 9, str(rep))
        c, rep = mux_connect(srv.port, 10, 1, swap16(5001)); c.close()
        r.check("Connect to wrong port -> Result 3", result_of(rep) == 3, str(rep))
        c, rep = mux_connect(srv.port, 11, 1, 5000); c.close()
        r.check("Connect with UNswapped PortNumber (5000) -> Result 3 (byte swap is enforced)",
                result_of(rep) == 3, str(rep))
        # Connect OK
        c, rep = mux_connect(srv.port, 12, 1, swap16(5000))
        r.check("Connect to port 5000 (PortNumber=%d) -> Result 0" % swap16(5000),
                result_of(rep) == 0 and rep[0] == 12, str(rep))
        t = TunnelClient(c.sock)

        # HELLO
        t.cmd(CMD_HELLO)
        pk = t.pump(2.0, lambda p: {T_HELLO, T_STATE, T_ORIENT, T_CAMERA} <= {x.type for x in p})
        hello = _of(pk, T_HELLO)
        r.check("HELLO -> HELLO(v3), STATE(idle), ORIENT, CAMERA(width 0)",
                bool(hello) and struct.unpack(">H", hello[0].payload)[0] == 3 and
                _of(pk, T_STATE)[0].payload[0] == ST_IDLE and bool(_of(pk, T_ORIENT)) and
                parse_camera(_of(pk, T_CAMERA)[0].payload)["width"] == 0,
                "types=%s" % [TYPE_NAMES[x.type] for x in pk])
        t.cmd(CMD_HELLO)
        pk2 = t.pump(0.5, lambda p: T_HELLO in {x.type for x in p})
        r.check("HELLO is idempotent (answers again)", bool(_of(pk2, T_HELLO)))

        # START
        t.cmd(CMD_START)
        pk = t.pump(5.0, lambda p: len(_of(p, T_FRAME)) >= 20)
        frames = _of(pk, T_FRAME)
        i_cfg = _first_idx(pk, lambda p: p.type == T_CONFIG)
        i_st = _first_idx(pk, lambda p: p.type == T_STATE and p.payload[0] == ST_STREAMING)
        i_fr = _first_idx(pk, lambda p: p.type == T_FRAME)
        r.check("START -> CONFIG, STATE(streaming), then FRAMEs (in that order)",
                0 <= i_cfg < i_st < i_fr, "idx cfg=%d state=%d frame=%d" % (i_cfg, i_st, i_fr))
        cfg = pk[i_cfg].payload if i_cfg >= 0 else b""
        w, h, o, f = struct.unpack(">HHHB", cfg[:7]) if len(cfg) >= 7 else (0, 0, 0, 9)
        r.check("CONFIG carries sample size + SPS/PPS", (w, h) == (sample.width, sample.height) and
                cfg[7:] == sample.sps_pps and f == 0, "w=%d h=%d facing=%d" % (w, h, f))
        r.check(">= 20 FRAMEs received", len(frames) >= 20, "got %d" % len(frames))
        r.check("first FRAME is a key frame", bool(frames) and bool(frames[0].flags & FLAG_KEYFRAME))
        r.check("PTS strictly increasing, also across sample loops (%d frames > %d in sample)" %
                (len(frames), len(sample.units)),
                len(frames) > len(sample.units) and all(b.pts > a.pts for a, b in zip(frames, frames[1:])))
        r.check("key frames recur on every loop", sum(1 for x in frames if x.flags & FLAG_KEYFRAME) >= 4)
        r.check("FRAME payloads are Annex-B", all(x.payload.startswith(SC4) for x in frames))
        cams = _of(pk, T_CAMERA)
        r.check("CAMERA after START reports the real size",
                bool(cams) and parse_camera(cams[-1].payload)["width"] == sample.width)

        # KEYFRAME
        t.cmd(CMD_KEYFRAME)
        pk = t.pump(2.0, lambda p: (lambda i: i >= 0 and any(x.type == T_FRAME for x in p[i:]))(
            _first_idx(p, lambda x: x.type == T_CONFIG)))
        i = _first_idx(pk, lambda p: p.type == T_CONFIG)
        after = [x for x in pk[i:] if x.type == T_FRAME] if i >= 0 else []
        r.check("KEYFRAME -> CONFIG, then a key frame", bool(after) and bool(after[0].flags & FLAG_KEYFRAME))

        # SET_FACING
        t.cmd(CMD_SET_FACING, 1)
        pk = t.pump(2.0, lambda p: any(x.type == T_STATE and x.payload[1] == 1 for x in p) and
                    any(x.type == T_CONFIG and x.payload[6] == 1 for x in p))
        r.check("SET_FACING(front) -> STATE and CONFIG report facing 1",
                any(x.type == T_STATE and x.payload[1] == 1 for x in pk) and
                any(x.type == T_CONFIG and x.payload[6] == 1 for x in pk))

        # v3 camera controls: wait for a CAMERA packet carrying the *new* value, because the packets
        # of the earlier camera restart may still be in flight.
        def camera_has(pred):
            return lambda p: any(x.type == T_CAMERA and pred(parse_camera(x.payload)) for x in p)

        checks = []
        for cmd, arg, key, want in [(CMD_SET_ZOOM, 20, "zoom", 200), (CMD_SET_EXPOSURE, 0xFE, "ev", -2),
                                    (CMD_SET_QUALITY, 2, "quality", 2), (CMD_SET_FPS, 60, "fps", 60)]:
            t.cmd(cmd, arg)
            pk = t.pump(2.0, camera_has(lambda c, key=key, want=want: c[key] == want))
            checks.append(camera_has(lambda c, key=key, want=want: c[key] == want)(pk))
        t.cmd(CMD_SET_TORCH, 1)
        pk = t.pump(2.0, camera_has(lambda c: bool(c["flags"] & CAM_TORCH_ON)))
        checks.append(camera_has(lambda c: bool(c["flags"] & CAM_TORCH_ON))(pk))
        t.cmd(CMD_SET_FOCUS, 1)
        pk = t.pump(2.0, camera_has(lambda c: bool(c["flags"] & CAM_FOCUS_LOCKED)))
        checks.append(camera_has(lambda c: bool(c["flags"] & CAM_FOCUS_LOCKED))(pk))
        r.check("zoom/exposure/quality/fps/torch/focus commands are reflected in CAMERA", all(checks), str(checks))
        t.pump(0.3)  # drain restart traffic

        # PAUSE / START while paused / RESUME / STOP
        t.cmd(CMD_PAUSE)
        pk = t.pump(2.0, lambda p: any(x.type == T_STATE and x.payload[0] == ST_PAUSED for x in p))
        i = _first_idx(pk, lambda p: p.type == T_STATE and p.payload[0] == ST_PAUSED)
        late = t.pump(0.4)
        r.check("PAUSE -> STATE(paused) and no FRAME afterwards", i >= 0 and
                not any(x.type == T_FRAME for x in pk[i:]) and not any(x.type == T_FRAME for x in late))
        t.cmd(CMD_START)
        pk = t.pump(0.6)
        r.check("START while paused -> STATE(paused), no CONFIG, no video",
                any(x.type == T_STATE and x.payload[0] == ST_PAUSED for x in pk) and
                not any(x.type in (T_CONFIG, T_FRAME) for x in pk))
        t.cmd(CMD_RESUME)
        pk = t.pump(3.0, lambda p: len(_of(p, T_FRAME)) >= 3)
        i_cfg = _first_idx(pk, lambda p: p.type == T_CONFIG)
        i_st = _first_idx(pk, lambda p: p.type == T_STATE and p.payload[0] == ST_STREAMING)
        i_fr = _first_idx(pk, lambda p: p.type == T_FRAME)
        r.check("RESUME (PC still wants video) -> CONFIG, STATE(streaming), FRAMEs again",
                0 <= i_cfg < i_st < i_fr and bool(pk[i_fr].flags & FLAG_KEYFRAME))
        t.cmd(CMD_STOP)
        pk = t.pump(2.0, lambda p: any(x.type == T_STATE and x.payload[0] == ST_IDLE for x in p))
        i = _first_idx(pk, lambda p: p.type == T_STATE and p.payload[0] == ST_IDLE)
        late = t.pump(0.4)
        r.check("STOP -> STATE(idle) and no FRAME afterwards", i >= 0 and
                not any(x.type == T_FRAME for x in pk[i:]) and not any(x.type == T_FRAME for x in late))

        # Detach / re-attach
        srv.detach()
        det, _ = l1.recv_until(lambda m: m[1].get("MessageType") == "Detached", 2.0)
        t.pump(1.0)
        r.check("detach -> Detached event to Listen client and tunnel closed",
                bool(det) and det[1].get("DeviceID") == 1 and t.closed)
        c2, rep = mux_connect(srv.port, 13, 1, swap16(5000)); c2.close()
        r.check("Connect while unplugged -> Result 2", result_of(rep) == 2, str(rep))
        srv._attach()
        att, _ = l1.recv_until(lambda m: m[1].get("MessageType") == "Attached", 2.0)
        r.check("re-attach -> Attached event again", bool(att))
        c3, rep = mux_connect(srv.port, 14, 1, swap16(5000))
        r.check("Connect after re-attach -> Result 0", result_of(rep) == 0, str(rep))
        c3.close()
        l1.close()
    finally:
        srv.stop()

    print("== 4. Flags: --refuse-connect-count / --attach-delay / --detach-after + --reattach-after")
    srv = Server(port=0, sample=sample, refuse_connect_count=2)
    srv.start()
    try:
        results = []
        for n in range(3):
            c, rep = mux_connect(srv.port, 20 + n, 1, swap16(5000))
            results.append(result_of(rep))
            c.close()
        r.check("--refuse-connect-count 2 -> Results 3, 3, 0", results == [3, 3, 0], str(results))
    finally:
        srv.stop()

    srv = Server(port=0, sample=sample, attach_delay=0.6)
    srv.start()
    try:
        l = MuxClient(srv.port)
        l.send(1, {"MessageType": "Listen", "ClientVersionString": "selftest", "ProgName": "selftest"})
        rep = l.recv()
        early = l.recv(0.3)
        c, cr = mux_connect(srv.port, 2, 1, swap16(5000)); c.close()
        late, _ = l.recv_until(lambda m: m[1].get("MessageType") == "Attached", 2.0)
        r.check("--attach-delay: Result first, Attached only after the delay, Connect before it -> 2",
                result_of(rep) == 0 and early is None and result_of(cr) == 2 and bool(late),
                "early=%s connect=%s late=%s" % (early, cr, late))
        l.close()
    finally:
        srv.stop()

    srv = Server(port=0, sample=sample, detach_after=0.5, reattach_after=0.5)
    srv.start()
    try:
        l = MuxClient(srv.port)
        l.send(1, {"MessageType": "Listen", "ClientVersionString": "selftest", "ProgName": "selftest"})
        l.recv()
        l.recv()  # initial Attached
        c, rep = mux_connect(srv.port, 2, 1, swap16(5000))
        t = TunnelClient(c.sock)
        det, _ = l.recv_until(lambda m: m[1].get("MessageType") == "Detached", 3.0)
        t.pump(0.5)
        att, _ = l.recv_until(lambda m: m[1].get("MessageType") == "Attached", 3.0)
        r.check("--detach-after 0.5 --reattach-after 0.5 -> Detached (tunnel closed), then Attached",
                result_of(rep) == 0 and bool(det) and t.closed and bool(att))
        l.close()
        c.close()
    finally:
        srv.stop()

    print("\n%d passed, %d failed" % (r.passed, r.failed))
    return 0 if r.failed == 0 else 1


# --------------------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------------------

def main() -> int:
    global LOG_ENABLED
    ap = argparse.ArgumentParser(description="Fake usbmuxd + fake iPhone for testing MyCam's iPhone transport.")
    ap.add_argument("--port", type=int, default=DEFAULT_MUX_PORT, help="usbmuxd port to listen on (default 27015)")
    ap.add_argument("--device-port", type=int, default=DEFAULT_DEVICE_PORT,
                    help="port the fake iOS app 'listens' on; must match the companion (default 5000)")
    ap.add_argument("--sample", help="H.264 Annex-B file to stream (default pc/tools/assets/sample.h264)")
    ap.add_argument("--fps", type=float, default=30.0, help="frame rate of --sample (default 30)")
    ap.add_argument("--synthetic", action="store_true",
                    help="use a built-in stream that is structurally valid H.264 but not decodable (no ffmpeg needed)")
    ap.add_argument("--attach-delay", type=float, default=0.0, metavar="SECONDS",
                    help="send Attached this long after the first Listen request")
    ap.add_argument("--detach-after", type=float, metavar="SECONDS",
                    help="once, this long after the tunnel is established: send Detached and close the tunnel")
    ap.add_argument("--reattach-after", type=float, metavar="SECONDS",
                    help="after a detach, send Attached again after this long (tests replug recovery)")
    ap.add_argument("--refuse-connect-count", type=int, default=0, metavar="N",
                    help="answer the first N Connect requests with Result 3 (connection refused)")
    ap.add_argument("--selftest", action="store_true", help="run built-in checks (no companion needed) and exit")
    ap.add_argument("--golden", help="path to golden.txt (default protocol/golden.txt)")
    ap.add_argument("--verbose", action="store_true", help="with --selftest: show the server log too")
    args = ap.parse_args()

    if args.selftest:
        return selftest(args)

    sample = load_sample(Path(args.sample) if args.sample else DEFAULT_SAMPLE, args.synthetic, args.fps)
    srv = Server(port=args.port, device_port=args.device_port, sample=sample,
                 detach_after=args.detach_after, reattach_after=args.reattach_after,
                 attach_delay=args.attach_delay, refuse_connect_count=args.refuse_connect_count)
    try:
        srv.start()
    except PortInUse:
        print('Port %d is in use. Stop Apple\'s service first: net stop "Apple Mobile Device Service"' % args.port)
        print("(See what holds it with: netstat -ano | findstr %d)" % args.port)
        return 1
    log("fake usbmuxd listening on 127.0.0.1:%d, fake iPhone app on device port %d" % (srv.port, args.device_port))
    log("sample: " + sample.describe())
    log("start MyCamCompanion.exe, then open the Windows Camera app and pick MyCam. Ctrl+C to quit.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        log("bye")
    finally:
        srv.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
