#include "frame_reader.h"

#include <sddl.h>
#include <string.h>
#include <algorithm>

#include "../common/shared_frame.h"

namespace mycam {

namespace {

constexpr uint64_t kStaleMs = 1500;

void FillBlack(uint8_t* dst, LONG pitch, UINT32 width, UINT32 height) {
    for (UINT32 y = 0; y < height; ++y) memset(dst + y * pitch, 16, width);
    uint8_t* uv = dst + pitch * height;
    for (UINT32 y = 0; y < height / 2; ++y) memset(uv + y * pitch, 128, width);
}

// Maps destination pixel (u, v) in the rotated/mirrored image of size ew x eh back to source (sx, sy)
// in the original w x h image.
inline void Inverse(UINT32 rot, int u, int v, int w, int h, int* sx, int* sy) {
    switch (rot) {
    case 90:  *sx = v;         *sy = h - 1 - u; break;
    case 180: *sx = w - 1 - u; *sy = h - 1 - v; break;
    case 270: *sx = w - 1 - v; *sy = u;         break;
    default:  *sx = u;         *sy = v;         break;
    }
}

} // namespace

FrameReader::FrameReader() = default;

FrameReader::~FrameReader() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
}

bool FrameReader::EnsureMapping() {
    if (view_) return true;

    // Running inside the Frame Server service we can create Global objects; elsewhere just try to open.
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kSharedSddl, SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, kSharedBytes,
                                  kSharedFrameName);
    if (sd) LocalFree(sd);
    if (!mapping_) mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedFrameName);
    if (!mapping_) return false;

    view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSharedBytes);
    if (!view_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (hdr->magic != kSharedMagic) {
        // Fresh section: initialise the header. The companion never writes frames before magic is set.
        hdr->version = kSharedVersion;
        hdr->seq = 0;
        hdr->width = hdr->height = 0;
        MemoryBarrier();
        hdr->magic = kSharedMagic;
    }
    return true;
}

bool FrameReader::CopyLatest() {
    if (!EnsureMapping()) return false;
    auto* hdr = static_cast<SharedHeader*>(view_);
    hdr->consumerTick = GetTickCount64();

    for (int attempt = 0; attempt < 3; ++attempt) {
        LONG seq1 = hdr->seq;
        if (seq1 & 1) { YieldProcessor(); continue; }
        if (seq1 == lastSeq_) return true; // Nothing new; keep the frame we have.
        MemoryBarrier();
        UINT32 w = hdr->width, h = hdr->height;
        if (w == 0 || h == 0 || w > kMaxWidth || h > kMaxHeight || (w | h) & 1) return false;
        size_t bytes = size_t(w) * h * 3 / 2;
        if (frame_.size() < bytes) frame_.resize(bytes);
        memcpy(frame_.data(), FrameData(view_), bytes);
        UINT32 rot = hdr->rotation;
        bool mirror = hdr->mirror != 0;
        uint64_t tick = hdr->frameTick;
        MemoryBarrier();
        if (hdr->seq != seq1) continue; // Torn read; retry.
        frameWidth_ = w;
        frameHeight_ = h;
        rotation_ = (rot == 90 || rot == 180 || rot == 270) ? rot : 0;
        mirror_ = mirror;
        frameTick_ = tick;
        lastSeq_ = seq1;
        return true;
    }
    return frameWidth_ != 0;
}

void FrameReader::Render(uint8_t* dst, LONG pitch, UINT32 width, UINT32 height) {
    bool have = CopyLatest() && frameWidth_ && GetTickCount64() - frameTick_ < kStaleMs;
    FillBlack(dst, pitch, width, height);
    if (!have) return;

    const int w = int(frameWidth_), h = int(frameHeight_);
    const bool swap = rotation_ == 90 || rotation_ == 270;
    const int ew = swap ? h : w, eh = swap ? w : h;

    // Fit (letterbox) the rotated image into the output, keeping even offsets/sizes for NV12.
    double scale = std::min(double(width) / ew, double(height) / eh);
    int dw = std::max(2, int(ew * scale) & ~1), dh = std::max(2, int(eh * scale) & ~1);
    int x0 = ((int(width) - dw) / 2) & ~1, y0 = ((int(height) - dh) / 2) & ~1;

    // Luma: precompute effective-image coordinates for each output column/row.
    xmap_.resize(dw);
    ymap_.resize(dh);
    for (int x = 0; x < dw; ++x) {
        int u = std::min(ew - 1, int(x * double(ew) / dw));
        xmap_[x] = mirror_ ? ew - 1 - u : u;
    }
    for (int y = 0; y < dh; ++y) ymap_[y] = std::min(eh - 1, int(y * double(eh) / dh));

    const uint8_t* srcY = frame_.data();
    for (int y = 0; y < dh; ++y) {
        uint8_t* row = dst + (y0 + y) * pitch + x0;
        int v = ymap_[y];
        for (int x = 0; x < dw; ++x) {
            int sx, sy;
            Inverse(rotation_, xmap_[x], v, w, h, &sx, &sy);
            row[x] = srcY[sy * w + sx];
        }
    }

    // Chroma at half resolution: work in UV-sample units of the source (w/2 x h/2).
    const uint8_t* srcUV = frame_.data() + size_t(w) * h;
    const int cw = w / 2, ch = h / 2, cew = ew / 2, ceh = eh / 2;
    uint8_t* dstUV = dst + pitch * height;
    for (int y = 0; y < dh / 2; ++y) {
        uint8_t* row = dstUV + (y0 / 2 + y) * pitch + x0;
        int v = std::min(ceh - 1, int(y * double(ceh) / (dh / 2)));
        for (int x = 0; x < dw / 2; ++x) {
            int u = std::min(cew - 1, int(x * double(cew) / (dw / 2)));
            if (mirror_) u = cew - 1 - u;
            int sx, sy;
            Inverse(rotation_, u, v, cw, ch, &sx, &sy);
            const uint8_t* s = srcUV + sy * w + sx * 2;
            row[x * 2] = s[0];
            row[x * 2 + 1] = s[1];
        }
    }
}

} // namespace mycam
