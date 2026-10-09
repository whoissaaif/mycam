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
#include "../companion/protocol.h"
#include "../vcam/frame_transform.h"

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

} // namespace

int main() {
    auto golden = LoadGolden();
    CHECK(!golden.empty());

    auto withGolden = [&](void (*fn)(std::map<std::string, Bytes>&)) { fn(golden); };

    printf("protocol golden packets\n");   withGolden(TestGoldenPackets);
    printf("protocol golden commands\n");  withGolden(TestGoldenCommands);
    printf("protocol golden camera\n");    withGolden(TestGoldenCamera);
    printf("parser splits/batches\n");     withGolden(TestParserSplitsAndBatches);
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

    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
