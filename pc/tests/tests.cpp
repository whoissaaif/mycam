// Unit tests for the pure parts of the PC side: packet parsing, command encoding (checked against
// protocol/golden.txt), and the NV12 rotate/scale/letterbox used by the virtual camera.
// Run: ctest --test-dir build -C Release   (or build\Release\mycam_tests.exe)

#include <stdio.h>
#include <string.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../companion/packet_parser.h"
#include "../companion/marquee.h"
#include "../companion/protocol.h"
#include "../companion/wifi_crypto.h"
#include "../companion/capabilities.h"
#include "../companion/discovery.h"
#include "../companion/preview_convert.h"
#include "../companion/status_text.h"
#include "../companion/ui_layout.h"
#include "../companion/ui_model.h"
#include "../companion/ui_motion.h"
#include "../vcam/frame_transform.h"

#include <cmath>

#undef small // rpcndr.h (via status_text.h -> windows.h) defines it; the marquee tests use it as a name.

using namespace mycam;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

using Bytes = std::vector<uint8_t>;

Bytes Hex(std::string s) {
    Bytes out;
    std::string digits;
    for (char c : s) if (isxdigit(static_cast<unsigned char>(c))) digits += c;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) out.push_back(uint8_t(std::stoul(digits.substr(i, 2), nullptr, 16)));
    return out;
}

std::map<std::string, Bytes> LoadGolden() {
    std::map<std::string, Bytes> golden;
    std::ifstream in(MYCAM_GOLDEN_PATH);
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        golden[name] = Hex(line.substr(eq + 1));
    }
    return golden;
}

std::vector<Packet> ParseAll(PacketParser& parser, const Bytes& data, std::vector<Bytes>* payloads = nullptr) {
    std::vector<Packet> packets;
    parser.Feed(data.data(), data.size(), [&](const Packet& p) {
        packets.push_back(p);
        if (payloads) payloads->emplace_back(p.payload, p.payload + p.length);
    });
    return packets;
}

// --- Protocol ---------------------------------------------------------------------------------

void TestGoldenPackets(std::map<std::string, Bytes>& golden) {
    struct Expect { const char* name; uint8_t type; uint8_t flags; int64_t pts; const char* payload; };
    const Expect cases[] = {
        {"packet.hello", proto::kHello, 0, 0, "0001"},
        {"packet.config", proto::kConfig, 0, 0, "0780 0438 005A 01 0000000167"},
        {"packet.frame", proto::kFrame, proto::kFlagKeyFrame, 0x0102030405060708LL, "DEADBEEF"},
        {"packet.orient", proto::kOrient, 0, 0, "010E"},
        {"packet.state", proto::kState, 0, 0, "01 01"},
        {"packet.log", proto::kLog, 0, 0, "6869"},
        {"packet.state_paused", proto::kState, 0, 0, "03 00"},
    };
    for (const Expect& e : cases) {
        CHECK(golden.count(e.name) == 1);
        PacketParser parser;
        std::vector<Bytes> payloads;
        auto packets = ParseAll(parser, golden[e.name], &payloads);
        CHECK(packets.size() == 1);
        if (packets.size() != 1) continue;
        CHECK(packets[0].type == e.type);
        CHECK(packets[0].flags == e.flags);
        CHECK(packets[0].ptsUs == e.pts);
        CHECK(payloads[0] == Hex(e.payload));
        CHECK(parser.Buffered() == 0);
    }
    // Spot-check the constants the golden payloads rely on.
    CHECK(Hex("01 01")[0] == proto::kStateStreaming);
    CHECK(Hex("01 01")[1] == proto::kFacingFront);
    CHECK(Hex("03 00")[0] == proto::kStatePaused);
}

void TestGoldenCommands(std::map<std::string, Bytes>& golden) {
    struct Expect { const char* name; proto::Command cmd; uint8_t arg; };
    const Expect cases[] = {
        {"command.hello", proto::kCmdHello, 0},
        {"command.start", proto::kCmdStart, 0},
        {"command.stop", proto::kCmdStop, 0},
        {"command.keyframe", proto::kCmdKeyFrame, 0},
        {"command.facing_front", proto::kCmdSetFacing, proto::kFacingFront},
        {"command.pause", proto::kCmdPause, 0},
        {"command.resume", proto::kCmdResume, 0},
        {"command.quality_4k", proto::kCmdSetQuality, proto::kQuality4K},
        {"command.fps_60", proto::kCmdSetFps, 60},
        {"command.fps_120", proto::kCmdSetFps, 120},
        {"command.zoom_2x", proto::kCmdSetZoom, 20},
        {"command.exposure_m2", proto::kCmdSetExposure, uint8_t(int8_t(-2))},
        {"command.torch_on", proto::kCmdSetTorch, 1},
        {"command.focus_lock", proto::kCmdSetFocus, 1},
    };
    for (const Expect& e : cases) {
        uint8_t out[proto::kCommandSize];
        proto::MakeCommand(out, e.cmd, e.arg);
        CHECK(Bytes(out, out + sizeof(out)) == golden[e.name]);
    }
}

void TestGoldenCamera(std::map<std::string, Bytes>& golden) {
    PacketParser parser;
    std::vector<Bytes> payloads;
    auto packets = ParseAll(parser, golden["packet.camera"], &payloads);
    CHECK(packets.size() == 1 && packets[0].type == proto::kCamera);
    proto::CameraInfo c;
    CHECK(!payloads.empty() && proto::ParseCameraInfo(payloads[0].data(), uint32_t(payloads[0].size()), &c));
    CHECK(c.quality == proto::kQuality1080p && c.fps == 30);
    CHECK(c.zoomX100 == 100 && c.zoomMinX100 == 60 && c.zoomMaxX100 == 1000);
    CHECK(c.ev == 0 && c.evMin == -12 && c.evMax == 12 && c.evStepX100 == 33);
    CHECK(c.flags == (proto::kCamTorchAvailable | proto::kCamHas60Fps | proto::kCamHas4K));
    CHECK(c.width == 1920 && c.height == 1080 && c.actualFps == 30);
    // Pre-1.3.2 payload: no per-quality masks, so they come from the overall flags (60 yes, 120 no).
    CHECK(!c.hasFpsModes && proto::FpsMask(c, proto::kQuality4K) == (proto::kFps30Bit | proto::kFps60Bit));

    payloads.clear();
    packets = ParseAll(parser, golden["packet.camera_modes"], &payloads);
    proto::CameraInfo m;
    CHECK(packets.size() == 1 && proto::ParseCameraInfo(payloads[0].data(), uint32_t(payloads[0].size()), &m));
    CHECK(m.fps == 120 && m.actualFps == 120 && (m.flags & proto::kCamHas120Fps));
    CHECK(m.hasFpsModes);
    CHECK(proto::FpsMask(m, proto::kQuality720p) == (proto::kFps30Bit | proto::kFps60Bit | proto::kFps120Bit));
    CHECK(proto::FpsMask(m, proto::kQuality1080p) == (proto::kFps30Bit | proto::kFps60Bit | proto::kFps120Bit));
    CHECK(proto::FpsMask(m, proto::kQuality4K) == proto::kFps30Bit); // 4K: 30 only, so 60/120 stay disabled.
}

void TestParserSplitsAndBatches(std::map<std::string, Bytes>& golden) {
    Bytes stream = golden["packet.hello"];
    Bytes frame = golden["packet.frame"];
    stream.insert(stream.end(), frame.begin(), frame.end());

    // One byte at a time.
    PacketParser parser;
    int count = 0;
    for (uint8_t b : stream) parser.Feed(&b, 1, [&](const Packet&) { ++count; });
    CHECK(count == 2);
    CHECK(parser.Buffered() == 0);

    // Both packets in a single read.
    PacketParser batch;
    CHECK(ParseAll(batch, stream).size() == 2);
}

void TestParserResyncs(std::map<std::string, Bytes>& golden) {
    // Garbage, a fake magic, and an absurd length must all be skipped.
    Bytes data = Hex("00 11 22 4D43414D 4D43414D 02 00 0000 0000000000000000 FFFFFFFF");
    Bytes hello = golden["packet.hello"];
    data.insert(data.end(), hello.begin(), hello.end());
    PacketParser parser;
    auto packets = ParseAll(parser, data);
    CHECK(packets.size() == 1);
    CHECK(!packets.empty() && packets[0].type == proto::kHello);
}

void TestParserReentrant(std::map<std::string, Bytes>& golden) {
    // A callback that feeds more data (as a nested USB event loop can) must not break parsing.
    Bytes hello = golden["packet.hello"];
    Bytes frame = golden["packet.frame"];
    PacketParser parser;
    std::vector<uint8_t> types;
    bool fed = false;
    std::function<void(const Packet&)> onPacket = [&](const Packet& p) {
        types.push_back(p.type);
        if (!fed) {
            fed = true;
            parser.Feed(frame.data(), frame.size(), onPacket);
        }
    };
    parser.Feed(hello.data(), hello.size(), onPacket);
    CHECK(types.size() == 2);
    CHECK(types.size() == 2 && types[0] == proto::kHello && types[1] == proto::kFrame);
    CHECK(parser.Buffered() == 0);
}

// --- Wireless crypto (golden.txt "wifi.*", made with the JDK) -----------------------------------

void TestWifiCrypto(std::map<std::string, Bytes>& golden) {
    using namespace mycam::wifi;
    CHECK(Hkdf(Hex(std::string(44, 'B').replace(0, 44, "0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B")), Hex("000102030405060708090A0B0C"),
               Hex("F0F1F2F3F4F5F6F7F8F9"), 42) == golden["wifi.hkdf_rfc5869_1"]);

    // ECDH both ways; also proves the CNG raw secret is byte-reversed into Java's order.
    const Bytes pcPub = golden["wifi.pc_public"], phonePub = golden["wifi.phone_public"];
    EcKey pc, phone;
    CHECK(pc.ImportPrivate(golden["wifi.pc_private"], pcPub));
    CHECK(phone.ImportPrivate(golden["wifi.phone_private"], phonePub));
    CHECK(pc.PublicRaw() == pcPub);
    Bytes z1, z2;
    CHECK(pc.Agree(phonePub, &z1) && z1 == golden["wifi.ecdh"]);
    CHECK(phone.Agree(pcPub, &z2) && z2 == golden["wifi.ecdh"]);
    Bytes bad = phonePub;
    bad[40] ^= 1; // Not on the curve any more: must be refused.
    CHECK(!pc.Agree(bad, &z1));

    const Bytes z = golden["wifi.ecdh"], na = golden["wifi.na"], nb = golden["wifi.nb"];
    CHECK(Commit(nb, phonePub, pcPub) == golden["wifi.commit"]);
    CHECK(Sas(pcPub, phonePub, na, nb) == "554294"); // golden "wifi.sas" holds these digits as text.
    const Bytes pairKey = PairKey(z, na, nb);
    CHECK(pairKey == golden["wifi.pair_key"]);
    const Bytes okm = SessionKeys(z, pairKey, golden["wifi.npc"], golden["wifi.nph"]);
    CHECK(okm == golden["wifi.session_okm"]);
    const Bytes kFin(okm.begin() + 64, okm.end());
    CHECK(Finished(kFin, "PC", golden["wifi.transcript"]) == golden["wifi.finished_pc"]);
    CHECK(Finished(kFin, "PH", golden["wifi.transcript"]) == golden["wifi.finished_phone"]);

    // Records: exact bytes, counters, tamper and direction checks.
    const Bytes k1(okm.begin(), okm.begin() + 32);
    RecordKey tx, rx;
    CHECK(tx.Init(k1, kDirPcToPhone));
    Bytes record;
    const char hello[] = "MCMD hello";
    CHECK(tx.Seal(reinterpret_cast<const uint8_t*>(hello), strlen(hello), &record));
    CHECK(record == golden["wifi.record_pc_0"]);

    RecordKey phoneTx;
    CHECK(phoneTx.Init(k1, kDirPhoneToPc));
    Bytes empty;
    for (int i = 0; i < 7; ++i) { Bytes skip; phoneTx.Seal(nullptr, 0, &skip); }
    CHECK(phoneTx.Seal(nullptr, 0, &empty) && empty == golden["wifi.record_phone_7"]);

    CHECK(rx.Init(k1, kDirPcToPhone));
    Bytes plain;
    CHECK(rx.Open(record.data() + 4, record.size() - 4, &plain) && Bytes(plain) == Bytes(hello, hello + strlen(hello)));
    CHECK(!rx.Open(record.data() + 4, record.size() - 4, &plain)); // Replayed: counter has moved on.
    RecordKey rx2;
    rx2.Init(k1, kDirPcToPhone);
    Bytes tampered = record;
    tampered[8] ^= 1;
    CHECK(!rx2.Open(tampered.data() + 4, tampered.size() - 4, &plain));
}

// --- Frame transform -------------------------------------------------------------------------

// A w x h NV12 image whose luma is a unique value per pixel (y * w + x + 1) and whose chroma pairs
// encode their UV coordinates, so every sampled pixel can be traced back.
Bytes MakeImage(int w, int h) {
    Bytes img(size_t(w) * h * 3 / 2);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img[y * w + x] = uint8_t(y * w + x + 1);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            img[w * h + y * w + x * 2] = uint8_t(100 + x);
            img[w * h + y * w + x * 2 + 1] = uint8_t(200 + y);
        }
    return img;
}

Bytes Render(const Bytes& src, int w, int h, uint32_t rot, bool mirror, int W, int H) {
    Bytes dst(size_t(W) * H * 3 / 2);
    FillBlackNV12(dst.data(), W, W, H);
    DrawFittedNV12(src.data(), w, h, rot, mirror, dst.data(), W, W, H);
    return dst;
}

void TestIdentity() {
    Bytes src = MakeImage(4, 4);
    CHECK(Render(src, 4, 4, 0, false, 4, 4) == src);
}

void TestRotations() {
    // 4x2 source:   1 2 3 4
    //               5 6 7 8
    Bytes src = MakeImage(4, 2);
    // 90 clockwise -> 2x4:  5 1 / 6 2 / 7 3 / 8 4
    Bytes r90 = Render(src, 4, 2, 90, false, 2, 4);
    CHECK((Bytes(r90.begin(), r90.begin() + 8) == Bytes{5, 1, 6, 2, 7, 3, 8, 4}));
    // 180 -> 8 7 6 5 / 4 3 2 1
    Bytes r180 = Render(src, 4, 2, 180, false, 4, 2);
    CHECK((Bytes(r180.begin(), r180.begin() + 8) == Bytes{8, 7, 6, 5, 4, 3, 2, 1}));
    // 270 clockwise -> 2x4:  4 8 / 3 7 / 2 6 / 1 5
    Bytes r270 = Render(src, 4, 2, 270, false, 2, 4);
    CHECK((Bytes(r270.begin(), r270.begin() + 8) == Bytes{4, 8, 3, 7, 2, 6, 1, 5}));
}

void TestMirror() {
    Bytes src = MakeImage(4, 2);
    Bytes m = Render(src, 4, 2, 0, true, 4, 2);
    CHECK((Bytes(m.begin(), m.begin() + 8) == Bytes{4, 3, 2, 1, 8, 7, 6, 5}));
    // Chroma mirrors too: UV columns 0,1 swap.
    CHECK(m[8] == 101 && m[10] == 100);
}

void TestLetterbox() {
    // A 2x4 (portrait) image into an 8x4 output: scaled 1:1, centred at x=3 -> even-aligned to x=2.
    Bytes src = MakeImage(2, 4);
    Bytes out = Render(src, 2, 4, 0, false, 8, 4);
    for (int y = 0; y < 4; ++y) {
        CHECK(out[y * 8 + 0] == 16 && out[y * 8 + 1] == 16);           // Left bar is black.
        CHECK(out[y * 8 + 2] == src[y * 2] && out[y * 8 + 3] == src[y * 2 + 1]); // Image.
        CHECK(out[y * 8 + 4] == 16 && out[y * 8 + 7] == 16);           // Right bar is black.
    }
    CHECK(out[8 * 4 + 0] == 128 && out[8 * 4 + 1] == 128); // Black chroma in the bar.
}

void TestFill() {
    // A 2x4 portrait image filling an 8x4 output: scaled 4x to 8x16, centred, so rows 6..9 of the scaled
    // image are visible = source rows 1..2. No black bars anywhere.
    Bytes src = MakeImage(2, 4); // luma: 1 2 / 3 4 / 5 6 / 7 8
    Bytes dst(8 * 4 * 3 / 2);
    FillBlackNV12(dst.data(), 8, 8, 4);
    DrawFittedNV12(src.data(), 2, 4, 0, false, dst.data(), 8, 8, 4, true);
    CHECK((Bytes(dst.begin(), dst.begin() + 8) == Bytes{3, 3, 3, 3, 4, 4, 4, 4}));
    CHECK((Bytes(dst.begin() + 24, dst.begin() + 32) == Bytes{5, 5, 5, 5, 6, 6, 6, 6}));
    bool anyBlack = false;
    for (size_t i = 0; i < 32; ++i) anyBlack |= dst[i] == 16;
    for (size_t i = 32; i < dst.size(); ++i) anyBlack |= dst[i] == 128;
    CHECK(!anyBlack);
    // Fill of a same-aspect image is identical to fit.
    Bytes sq = MakeImage(4, 4);
    CHECK(Render(sq, 4, 4, 0, false, 4, 4) == [&] {
        Bytes d(4 * 4 * 3 / 2);
        FillBlackNV12(d.data(), 4, 4, 4);
        DrawFittedNV12(sq.data(), 4, 4, 0, false, d.data(), 4, 4, 4, true);
        return d;
    }());
}

void TestDownscale() {
    // 8x8 -> 4x4 picks every other pixel.
    Bytes src = MakeImage(8, 8);
    Bytes out = Render(src, 8, 8, 0, false, 4, 4);
    CHECK(out[0] == src[0] && out[1] == src[2] && out[4] == src[16] && out[5] == src[18]);
}

void TestPitch() {
    // Destination rows wider than the image (padding) must not be written past the image width.
    const int W = 4, H = 2, pitch = 8;
    Bytes src = MakeImage(4, 2);
    Bytes dst(size_t(pitch) * H * 3 / 2, 0xAB);
    FillBlackNV12(dst.data(), pitch, W, H);
    DrawFittedNV12(src.data(), 4, 2, 0, false, dst.data(), pitch, W, H);
    CHECK(dst[0] == 1 && dst[3] == 4 && dst[pitch] == 5);
    CHECK(dst[4] == 0xAB && dst[pitch + 7] == 0xAB); // Padding untouched.
}

void TestBgraToNV12() {
    // 2x2 blocks of solid colours -> known BT.601 limited-range values.
    struct Case { uint8_t r, g, b, y, u, v; };
    const Case cases[] = {
        {255, 255, 255, 235, 128, 128}, // white
        {0, 0, 0, 16, 128, 128},        // black
        {255, 0, 0, 82, 90, 240},       // red
        {128, 128, 128, 126, 128, 128}, // mid gray
    };
    for (const Case& c : cases) {
        uint8_t bgra[16];
        for (int i = 0; i < 4; ++i) { bgra[i * 4] = c.b; bgra[i * 4 + 1] = c.g; bgra[i * 4 + 2] = c.r; bgra[i * 4 + 3] = 255; }
        uint8_t out[6] = {};
        BgraToNV12(bgra, 8, 2, 2, out);
        CHECK(out[0] == c.y && out[1] == c.y && out[2] == c.y && out[3] == c.y);
        CHECK(out[4] == c.u && out[5] == c.v);
    }
    // Chroma averages a 2x2 block: half black, half white -> neutral chroma.
    uint8_t mixed[16] = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    uint8_t out[6] = {};
    BgraToNV12(mixed, 8, 2, 2, out);
    CHECK(out[0] == 16 && out[1] == 235 && out[4] == 128 && out[5] == 128);
}

// --- Waiting-picture marquee ----------------------------------------------------------------------

Nv12Image PatternImage(uint32_t w, uint32_t h) {
    Nv12Image img;
    img.width = w;
    img.height = h;
    img.data.resize(size_t(w) * h * 3 / 2);
    for (size_t i = 0; i < img.data.size(); ++i) img.data[i] = uint8_t((i * 7 + i / w * 13) & 0xFF);
    return img;
}

// True if pixel (x, y) is covered by a visible chunk at time t (clipped to the interior).
bool InChunk(const MarqueeGeometry& g, double t, int x, int y) {
    if (y < g.iy || y >= g.iy + g.ih || x < g.ix || x >= g.ix + g.iw) return false;
    for (int i = 0; i < kMarqueeChunks; ++i) {
        int left = 0;
        if (MarqueeChunkLeft(g, t, i, &left) && x >= left && x < left + g.chunkW) return true;
    }
    return false;
}

void TestMarqueeGeometry() {
    MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    CHECK(g.valid());
    CHECK(g.x == kMarqueeTrackX && g.y == kMarqueeTrackY && g.w == kMarqueeTrackW && g.h == kMarqueeTrackH);
    CHECK(g.ix == g.x + 2 && g.iy == g.y + 2 && g.iw == g.w - 4 && g.ih == g.h - 4);
    CHECK(g.chunkW == 8 && g.gap == 2);
    MarqueeGeometry half = MarqueeGeometryFor(640, 360);
    CHECK(half.valid() && half.chunkW == 4 && half.gap == 1);
    CHECK(half.x == (kMarqueeTrackX + 1) / 2 && half.w == kMarqueeTrackW / 2);
    MarqueeGeometry big = MarqueeGeometryFor(1920, 1080);
    CHECK(big.chunkW == 12 && big.gap == 3 && big.x == kMarqueeTrackX * 3 / 2);
    CHECK(!MarqueeGeometryFor(0, 0).valid());
    CHECK(!MarqueeGeometryFor(16, 16).valid()); // Track falls outside a tiny image.
}

void TestMarqueeChunkPositions() {
    const MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    const int group = 3 * g.chunkW + 2 * g.gap; // 28
    int l0 = 0, l1 = 0, l2 = 0;
    // Start of a pass: the whole group sits just left of the interior.
    CHECK(MarqueeChunkLeft(g, 0.0, 0, &l0) && MarqueeChunkLeft(g, 0.0, 2, &l2));
    CHECK(l2 == g.ix - group && l0 + g.chunkW == g.ix);
    // Halfway: linear, chunk 0 leads, chunks 10 px apart.
    CHECK(MarqueeChunkLeft(g, 1.0, 0, &l0) && MarqueeChunkLeft(g, 1.0, 1, &l1) && MarqueeChunkLeft(g, 1.0, 2, &l2));
    CHECK(l2 == g.ix - group + (g.iw + group) / 2);
    CHECK(l1 == l2 + g.chunkW + g.gap && l0 == l1 + g.chunkW + g.gap);
    // Monotonic left to right over the pass, and the group ends past the right edge.
    int prev = -100000;
    bool monotonic = true;
    for (double t = 0; t < kMarqueePassSec; t += 0.01) {
        int l = 0;
        CHECK(MarqueeChunkLeft(g, t, 2, &l));
        if (l < prev) monotonic = false;
        prev = l;
    }
    CHECK(monotonic);
    CHECK(MarqueeChunkLeft(g, 1.9999, 2, &l2) && l2 >= g.ix + g.iw - 1);
    // Repeats every pass + pause.
    const double period = kMarqueePassSec + kMarqueePauseSec;
    CHECK(MarqueeChunkLeft(g, 1.0 + 5 * period, 0, &l1) && MarqueeChunkLeft(g, 1.0, 0, &l0) && l0 == l1);
    CHECK(!MarqueeChunkLeft(g, 1.0, 3, &l0)); // Only 3 chunks.
}

void TestMarqueePause() {
    const MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    int l = 0;
    CHECK(!MarqueeChunkLeft(g, 2.0, 0, &l));
    CHECK(!MarqueeChunkLeft(g, 2.2, 1, &l));
    CHECK(!MarqueeChunkLeft(g, 2.39, 2, &l));
    CHECK(MarqueeChunkLeft(g, 2.4, 0, &l));
    const Nv12Image base = PatternImage(1280, 720);
    Nv12Image img = base;
    DrawMarquee(img, base, 1.0);
    CHECK(img.data != base.data);
    DrawMarquee(img, base, 2.2); // Pause: the empty track (the base picture) comes back.
    CHECK(img.data == base.data);
}

void TestMarqueeDraw(uint32_t w, uint32_t h) {
    const MarqueeGeometry g = MarqueeGeometryFor(w, h);
    const Nv12Image base = PatternImage(w, h);
    const int x0 = g.x & ~1, y0 = g.y & ~1, x1 = (g.x + g.w + 1) & ~1, y1 = (g.y + g.h + 1) & ~1;
    const uint8_t* by = base.data.data();
    const uint8_t* buv = by + size_t(w) * h;
    for (double t : {0.05, 0.37, 1.0, 1.23, 1.71, 1.98}) {
        Nv12Image img = base;
        DrawMarquee(img, base, t);
        const uint8_t* y = img.data.data();
        const uint8_t* uv = y + size_t(w) * h;
        bool outsideClean = true, chunksDrawn = true, gapsClean = true, chromaOk = true;
        int painted = 0;
        for (uint32_t r = 0; r < h; ++r) {
            for (uint32_t c = 0; c < w; ++c) {
                const size_t i = size_t(r) * w + c;
                const bool inRect = int(c) >= x0 && int(c) < x1 && int(r) >= y0 && int(r) < y1;
                if (!inRect && y[i] != by[i]) outsideClean = false;
                if (InChunk(g, t, c, r)) ++painted;
                else if (inRect && y[i] != by[i]) gapsClean = false;
            }
        }
        // Chroma: changed only for whole blocks inside a chunk; every such block is painted green.
        for (uint32_t br = 0; br < h / 2; ++br) {
            for (uint32_t bx = 0; bx < w / 2; ++bx) {
                const size_t i = size_t(br) * w + 2 * bx;
                const bool full = InChunk(g, t, 2 * bx, 2 * br) && InChunk(g, t, 2 * bx + 1, 2 * br) &&
                                  InChunk(g, t, 2 * bx, 2 * br + 1) && InChunk(g, t, 2 * bx + 1, 2 * br + 1);
                const bool changed = uv[i] != buv[i] || uv[i + 1] != buv[i + 1];
                const bool inRect = int(2 * bx) >= x0 && int(2 * bx) < x1 && int(2 * br) >= y0 && int(2 * br) < y1;
                if (!inRect && changed) outsideClean = false;
                if (!full && inRect && changed) chromaOk = false;
                if (full && !(uv[i] < 128 && uv[i + 1] < 128)) chromaOk = false; // Green: U and V below neutral.
            }
        }
        // Painted pixels: lighter at the top highlight than at the dark 55 % band.
        for (int c = g.ix; c < g.ix + g.iw; ++c) {
            if (!InChunk(g, t, c, g.iy)) continue;
            if (!(y[size_t(g.iy) * w + c] > y[size_t(g.iy + g.ih * 55 / 100) * w + c])) chunksDrawn = false;
        }
        CHECK(outsideClean);
        CHECK(gapsClean);
        CHECK(chromaOk);
        CHECK(chunksDrawn);
        CHECK(painted > 0);
    }
}

void TestMarqueeRestore() {
    const Nv12Image base = PatternImage(1280, 720);
    Nv12Image img = base;
    // Successive frames only ever show the current chunks: earlier ones are wiped by the restore.
    DrawMarquee(img, base, 0.5);
    DrawMarquee(img, base, 1.5);
    Nv12Image fresh = base;
    DrawMarquee(fresh, base, 1.5);
    CHECK(img.data == fresh.data);
    // Garbage inside the track is restored too.
    const MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    for (int r = g.y; r < g.y + g.h; ++r) memset(img.data.data() + size_t(r) * 1280 + g.x, 0x55, g.w);
    DrawMarquee(img, base, 2.3);
    CHECK(img.data == base.data);
    // Mismatched sizes are ignored rather than overrun.
    Nv12Image small = PatternImage(640, 360);
    const Nv12Image before = small;
    DrawMarquee(small, base, 1.0);
    CHECK(small.data == before.data);
}

// --- Companion UI (layout, motion, capabilities, element model, preview, status text) ---------------

void TestUiFlow() {
    using namespace mycam::ui;
    float bottom = 0;
    // Three 60-wide items in 150: two on the first line, the third wraps.
    auto boxes = Flow({{60, 16}, {60, 16}, {60, 16}}, 10, 20, 150, 10, 4, &bottom);
    CHECK(boxes.size() == 3);
    CHECK(boxes[0].l == 10 && boxes[1].l == 80 && boxes[0].t == 20);
    CHECK(boxes[2].l == 10 && boxes[2].t == 40);
    CHECK(bottom == 56);
    for (size_t i = 0; i < boxes.size(); ++i) {
        CHECK(boxes[i].r <= 160.01f);
        for (size_t j = i + 1; j < boxes.size(); ++j) CHECK(!boxes[i].Intersects(boxes[j]));
    }
    // An item wider than the line gets its own line, clipped to the width; shorter items are centred.
    boxes = Flow({{300, 20}, {40, 10}, {40, 20}}, 0, 0, 100, 6, 0, &bottom);
    CHECK(boxes[0].W() == 100 && boxes[1].t == 20 + 5 && boxes[2].t == 20);
    CHECK(Flow({}, 0, 7, 100, 0, 0, &bottom).empty() && bottom == 7);
    // Split: fixed columns first, the flexible ones share the rest.
    auto cols = Split(0, 0, 100, 10, {20, 0, 0}, 10);
    CHECK(cols[0].W() == 20 && cols[1].W() == 30 && cols[2].l == 70 && cols[2].r == 100);
    Column c(5, 5, 50);
    Box a = c.Row(10);
    c.Gap(3);
    Box b = c.Item({20, 8}, 2);
    CHECK(a.t == 5 && b.t == 18 && b.l == 35 && c.Y() == 26);
}

void TestUiMotion() {
    using namespace mycam::ui;
    CHECK(EaseOut(0) == 0 && EaseOut(1) == 1 && EaseInOut(0.5f) > 0.49f && EaseInOut(0.5f) < 0.51f);
    CHECK(std::fabs(BackOut(1) - 1) < 1e-5f);
    bool overshoots = false;
    for (int i = 1; i < 20; ++i) overshoots |= BackOut(i / 20.f) > 1.f;
    CHECK(overshoots);
    Tween t;
    t.Start(1, 1000, 200, 0);
    CHECK(t.Running(1100) && !t.Running(1200) && t.Value(1000) == 0 && t.Value(1300) == 1);
    t.Start(0, 1000, 0, 1); // Reduced motion: instant.
    CHECK(!t.Running(1000) && t.Value(1000) == 0);
    // Marquee: snaps to chunk steps, crosses the whole track in 2.0 s, nothing during the 0.4 s pause.
    const float track = 200, chunk = 8, gap = 2;
    CHECK(MarqueeOffset(0, track, chunk, gap) == -30);
    for (uint64_t ms = 0; ms < 2000; ms += 37) {
        const float x = MarqueeOffset(ms, track, chunk, gap);
        CHECK(std::fmod(x, 10.f) == 0 && x >= -30 && x <= track);
    }
    CHECK(MarqueeOffset(2100, track, chunk, gap) >= track);
    CHECK(MarqueeOffset(2400, track, chunk, gap) == -30); // Next pass.
    CHECK(MarqueeOffset(1000, track, chunk, gap) > MarqueeOffset(500, track, chunk, gap));
    // Determinate: whole chunks only.
    CHECK(DeterminateChunks(1, 98, 8, 2) == 10 && DeterminateChunks(0, 98, 8, 2) == 0);
    CHECK(DeterminateChunks(0.55f, 98, 8, 2) == 5 && DeterminateChunks(2, 98, 8, 2) == 10);
    wchar_t text[8];
    FormatCountdown(42000, text, 8);
    CHECK(wcscmp(text, L"0:42") == 0);
    FormatCountdown(60000, text, 8);
    CHECK(wcscmp(text, L"1:00") == 0);
    FormatCountdown(1, text, 8);
    CHECK(wcscmp(text, L"0:01") == 0);
}

void TestCapabilities() {
    proto::CameraInfo c;
    CHECK(!caps::QualityEnabled(true, c, proto::kQuality720p)); // No CameraInfo yet.
    c.valid = true;
    CHECK(caps::QualityEnabled(true, c, proto::kQuality4K));   // Unknown until the camera starts: offered.
    CHECK(caps::FpsEnabled(true, c, 30) && !caps::FpsEnabled(true, c, 60));
    c.width = 1920;
    c.height = 1080;
    c.quality = proto::kQuality1080p;
    c.hasFpsModes = true;
    c.fpsModes[0] = proto::kFps30Bit | proto::kFps60Bit | proto::kFps120Bit;
    c.fpsModes[1] = proto::kFps30Bit | proto::kFps60Bit;
    c.fpsModes[2] = proto::kFps30Bit;
    CHECK(!caps::QualityEnabled(true, c, proto::kQuality4K)); // Known, and no 4K flag.
    CHECK(caps::FpsEnabled(true, c, 60) && !caps::FpsEnabled(true, c, 120));
    c.fps = 120;
    CHECK(caps::EffectiveFps(c) == 60); // 120 asked, 60 is the best this quality does.
    c.zoomX100 = 100; c.zoomMinX100 = 60; c.zoomMaxX100 = 500;
    CHECK(caps::ZoomOutEnabled(true, c) && caps::ZoomInEnabled(true, c) && !caps::ZoomResetEnabled(true, c));
    CHECK(!caps::AutoEnabled(true, c));
    c.ev = 1;
    CHECK(caps::AutoEnabled(true, c) && !caps::EvUpEnabled(true, c)); // evMax == evMin: no exposure control.
    CHECK(!caps::QualityEnabled(false, c, proto::kQuality720p));
    CHECK(wcscmp(caps::QualityLabel(1080), L"1080p") == 0 && wcscmp(caps::QualityLabel(2160), L"4K") == 0);
}

void TestUiModel() {
    using namespace mycam::ui;
    auto make = [](int id, Kind kind, int parent, int set = kSetNone, bool selected = false, bool enabled = true) {
        Element e;
        e.id = id;
        e.kind = kind;
        e.parent = parent;
        e.radioSet = set;
        e.selected = selected;
        e.enabled = enabled;
        e.focusable = kind != Kind::Text;
        return e;
    };
    std::vector<Element> list = {
        make(kPause, Kind::Button, kGroupNow),
        make(kStatusHeadline, Kind::Text, kGroupNow),
        make(kBack, Kind::Radio, kGroupCamera, kSetFacing, false),
        make(kFront, Kind::Radio, kGroupCamera, kSetFacing, true),
        make(kQ720, Kind::Radio, kGroupVideo, kSetQuality, false),
        make(kQ1080, Kind::Radio, kGroupVideo, kSetQuality, false),
        make(kQ4K, Kind::Radio, kGroupVideo, kSetQuality, false, false),
        make(kMirror, Kind::Checkbox, kGroupPicture),
        make(kFill, Kind::Checkbox, kGroupPicture),
    };
    list[7].name = L"Mirror the image";
    list[7].accessKeyIndex = 0;
    // A radio set is one tab stop: its selected option, else its first enabled one.
    const std::vector<int> stops = TabStops(list);
    CHECK((stops == std::vector<int>{kPause, kFront, kQ720, kMirror, kFill}));
    CHECK(NextTabStop(list, kPause, false) == kFront);
    CHECK(NextTabStop(list, kBack, false) == kQ720);   // From either option of the set.
    CHECK(NextTabStop(list, kFill, false) == kPause);  // Wraps.
    CHECK(NextTabStop(list, kPause, true) == kFill);
    CHECK(NextTabStop(list, kNone, false) == kPause);
    // Arrows stay inside the set and skip disabled options.
    CHECK(ArrowTarget(list, kQ1080, 1) == kQ720);
    CHECK(ArrowTarget(list, kQ720, -1) == kQ1080);
    CHECK(ArrowTarget(list, kFront, 1) == kBack);
    CHECK(ArrowTarget(list, kMirror, 1) == kFill && ArrowTarget(list, kFill, 1) == kMirror);
    // Hidden (collapsed) elements are skipped.
    list[7].visible = false;
    CHECK(NextTabStop(list, kQ720, false) == kFill);
    CHECK(AccessKeyTarget(list, L'm') == kNone);
    list[7].visible = true;
    CHECK(AccessKeyTarget(list, L'M') == kMirror && AccessKeyTarget(list, L'x') == kNone);
    int key = -2;
    CHECK(StripAccessKey(L"&Pause the camera", &key) == L"Pause the camera" && key == 0);
    CHECK(StripAccessKey(L"Fish && chips", &key) == L"Fish & chips" && key == -1);
    Element off = make(kOpenLog, Kind::Link, kGroupTasks);
    off.rect = {0, 600, 50, 615};
    off.clip = {0, 30, 240, 497};
    CHECK(off.Offscreen());
    off.rect = {0, 400, 50, 415};
    CHECK(!off.Offscreen());
}

void TestDiscovery() {
    // Parsing the phone's answer: "MYCAM!1 <tcp port> <phone name>".
    uint16_t port = 0;
    std::string name = "x";
    CHECK(ParseDiscoveryReply("MYCAM!1 47800 Pixel 8", &port, &name) && port == 47800 && name == "Pixel 8");
    CHECK(ParseDiscoveryReply("MYCAM!1 47800", &port, &name) && port == 47800 && name.empty());
    CHECK(ParseDiscoveryReply("MYCAM!1 1 a  b \r\n", &port, &name) && port == 1 && name == "a  b");
    CHECK(!ParseDiscoveryReply("MYCAM?1 DESKTOP", &port, &name));   // Another PC's probe.
    CHECK(!ParseDiscoveryReply("MYCAM!2 47800 x", &port, &name));   // Another version.
    CHECK(!ParseDiscoveryReply("MYCAM!10 47800 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 0 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 65536 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 47a00 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 x", &port, &name));
    CHECK(ParseDiscoveryReply("MYCAM!1 65535 " + std::string(100, 'n'), &port, &name) && port == 65535 && name.size() == 64);

    // "Connect to me": the same body under its own tag, and never confused with an answer.
    CHECK(ParseDiscoveryConnectRequest("MYCAM+1 47800 Pixel 8", &port, &name) && port == 47800 && name == "Pixel 8");
    CHECK(ParseDiscoveryConnectRequest("MYCAM+1 47800", &port, &name) && port == 47800 && name.empty());
    CHECK(!ParseDiscoveryConnectRequest("MYCAM!1 47800 x", &port, &name));
    CHECK(!ParseDiscoveryConnectRequest("MYCAM+2 47800 x", &port, &name));
    CHECK(!ParseDiscoveryConnectRequest("MYCAM+1 0 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM+1 47800 x", &port, &name));

    // The list: one entry per address, in first-answer order, refreshed by later answers.
    NearbyList list;
    CHECK(list.Seen(1, "10.0.0.1", 47800, "Pixel", 1000));
    CHECK(list.Seen(2, "10.0.0.2", 47800, "", 1100));                   // No name: the address stands in.
    CHECK(!list.Seen(1, "10.0.0.1", 47800, "Pixel", 3000));             // Same answer again: no change.
    CHECK(list.Phones().size() == 2 && list.Phones()[0].lastSeen == 3000 && list.Phones()[1].name == "10.0.0.2");
    CHECK(list.Seen(1, "10.0.0.1", 47801, "Pixel", 3100));              // A new port is a change.
    CHECK(list.Find(1)->port == 47801 && !list.Find(3));
    // Expiry drops the phones that stopped answering, except the connected one.
    CHECK(list.SetConnected(2) && !list.SetConnected(2));
    CHECK(!list.Expire(7000, 6000));
    CHECK(list.Expire(9200, 6000) && list.Phones().size() == 1 && list.Phones()[0].ip == 2);
    CHECK(list.SetConnected(0) && !list.Phones()[0].connected);
    // Paired marks come from a name check.
    list.Seen(5, "10.0.0.5", 47800, "Moto", 9300);
    CHECK(list.UpdatePaired([](const std::string& n) { return n == "Moto"; }) && list.Find(5)->paired && !list.Find(2)->paired);
    CHECK(!list.UpdatePaired([](const std::string& n) { return n == "Moto"; }));
    // A new scan keeps only the connected phone.
    list.SetConnected(5);
    list.ClearForScan();
    CHECK(list.Phones().size() == 1 && list.Phones()[0].ip == 5);
    // Bounded: a 17th phone replaces the one heard from longest ago (never the connected one).
    list.Clear();
    for (uint32_t i = 0; i < NearbyList::kMax; ++i) list.Seen(100 + i, "ip", 1, "p", 10000 + i);
    list.SetConnected(100);
    CHECK(list.Seen(200, "ip", 1, "new", 20000) && list.Phones().size() == NearbyList::kMax);
    CHECK(list.Find(100) && !list.Find(101) && list.Find(200));
    CHECK(ScanResultSentence(0).rfind(L"No phone found.", 0) == 0);
    CHECK(ScanResultSentence(1) == L"Found 1 phone." && ScanResultSentence(2) == L"Found 2 phones.");
    // Scan rows have stable ids in their own block.
    int part = -1;
    CHECK(ui::ScanRowIndex(ui::ScanRowId(3, 1), &part) == 3 && part == 1);
    CHECK(ui::ScanRowIndex(ui::ScanRowId(15, 0), &part) == 15 && part == 0);
    CHECK(ui::ScanRowIndex(ui::kScanList, &part) == -1 && ui::ScanRowIndex(ui::ScanRowId(16, 0), &part) == -1);
}

void TestPreviewConvert() {
    // 4x2 NV12 -> 2x1 BGRA: each output pixel averages a 2x2 luma block.
    const uint8_t nv12[] = {16, 16, 235, 235, 16, 16, 235, 235, 128, 128, 128, 128};
    uint8_t out[8] = {};
    Nv12ToBgraHalf(nv12, 2, 1, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 255);
    CHECK(out[4] == 255 && out[5] == 255 && out[6] == 255 && out[7] == 255);
    // Pure red from BgraToNV12 comes back red.
    std::vector<uint8_t> bgra(4 * 4 * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) { bgra[i] = 0; bgra[i + 1] = 0; bgra[i + 2] = 255; bgra[i + 3] = 255; }
    std::vector<uint8_t> yuv(4 * 4 * 3 / 2);
    BgraToNV12(bgra.data(), 16, 4, 4, yuv.data());
    uint8_t back[2 * 2 * 4];
    Nv12ToBgraHalf(yuv.data(), 2, 2, back);
    CHECK(back[2] > 240 && back[1] < 15 && back[0] < 15);
}

void TestStatusText() {
    LinkStatus s;
    s.state = LinkState::Waiting;
    s.wireless = true;
    s.phoneName = L"Pixel 8";
    s.pairingCode = L"554 294";
    StatusView v = DescribeStatus(s, true);
    CHECK(v.headline == L"Pairing with Pixel 8…");
    CHECK(v.progress == StatusProgress::Pairing && v.facts.find(L"554 294") != std::wstring::npos);
    s.pairingCode.clear();
    CHECK(DescribeStatus(s, true).progress == StatusProgress::Working);
    s.state = LinkState::Streaming;
    s.width = 1920;
    s.height = 1080;
    v = DescribeStatus(s, true);
    CHECK(v.headline == L"Streaming" && v.progress == StatusProgress::None && v.icon == kIconStreaming);
    CHECK(v.facts == L"Back camera · 1920 × 1080 · Wi-Fi");
    s.state = LinkState::Idle;
    CHECK(DescribeStatus(s, true).progress == StatusProgress::None); // Never in steady states.
    s.lockPaused = true;
    CHECK(DescribeStatus(s, true).icon == kIconPaused);
    CHECK(DescribeStatus(s, false).icon == kIconError);
}

} // namespace

int main() {
    auto golden = LoadGolden();
    CHECK(!golden.empty());

    auto withGolden = [&](void (*fn)(std::map<std::string, Bytes>&)) { fn(golden); };

    printf("protocol golden packets\n");   withGolden(TestGoldenPackets);
    printf("protocol golden commands\n");  withGolden(TestGoldenCommands);
    printf("protocol golden camera\n");    withGolden(TestGoldenCamera);
    printf("parser splits/batches\n");     withGolden(TestParserSplitsAndBatches);
    printf("wireless crypto\n");           withGolden(TestWifiCrypto);
    printf("parser resyncs\n");            withGolden(TestParserResyncs);
    printf("parser re-entrant\n");         withGolden(TestParserReentrant);
    printf("transform identity\n");        TestIdentity();
    printf("transform rotations\n");       TestRotations();
    printf("transform mirror\n");          TestMirror();
    printf("transform letterbox\n");       TestLetterbox();
    printf("transform fill\n");            TestFill();
    printf("transform downscale\n");       TestDownscale();
    printf("transform pitch\n");           TestPitch();
    printf("bgra to nv12\n");              TestBgraToNV12();
    printf("marquee geometry\n");          TestMarqueeGeometry();
    printf("marquee chunk positions\n");   TestMarqueeChunkPositions();
    printf("marquee pause\n");             TestMarqueePause();
    printf("marquee draw 1280x720\n");     TestMarqueeDraw(1280, 720);
    printf("marquee draw 1920x1080\n");    TestMarqueeDraw(1920, 1080);
    printf("marquee draw 854x480\n");      TestMarqueeDraw(854, 480);
    printf("marquee restore\n");           TestMarqueeRestore();
    printf("ui flow layout\n");            TestUiFlow();
    printf("ui motion\n");                 TestUiMotion();
    printf("ui capabilities\n");           TestCapabilities();
    printf("ui element model\n");          TestUiModel();
    printf("wi-fi discovery list\n");      TestDiscovery();
    printf("preview convert\n");           TestPreviewConvert();
    printf("status text\n");               TestStatusText();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
