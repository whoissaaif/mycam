#include "preview_source.h"

#include <sddl.h>

#include "../common/shared_frame.h"
#include "../vcam/frame_transform.h"
#include "preview_convert.h"

namespace mycam {

namespace {
constexpr uint64_t kStaleMs = 1500;  // The virtual camera's own staleness rule.
constexpr uint64_t kRetryOpenMs = 500;
} // namespace

void PreviewSource::Close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    view_ = nullptr;
    mapping_ = nullptr;
    lastSeq_ = -1;
    writable_ = false;
    onLocal_ = false;
}

bool PreviewSource::OpenNamed(const wchar_t* name, bool local) {
    HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!m) m = OpenFileMappingW(FILE_MAP_READ, FALSE, name); // Read-only still shows frames.
    if (!m && local) {
        // Nobody has made the fallback section yet (no app has the camera open, and the companion has not
        // written a frame since). Create it the way frame_writer.cpp / frame_reader.cpp do.
        PSECURITY_DESCRIPTOR sd = nullptr;
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kSharedSddl, SDDL_REVISION_1, &sd, nullptr)) {
            sa.lpSecurityDescriptor = sd;
        }
        m = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, kSharedBytes, name);
        if (sd) LocalFree(sd);
    }
    if (!m) return false;
    void* v = MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSharedBytes);
    const bool writable = v != nullptr;
    if (!v) v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, kSharedBytes);
    if (!v) {
        CloseHandle(m);
        return false;
    }
    Close();
    mapping_ = m;
    view_ = v;
    writable_ = writable;
    onLocal_ = local;
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (local && writable_ && hdr->magic != kSharedMagic) {
        // Fresh section: initialise the header, as the virtual camera does for Global\ (frame_reader.cpp).
        // The companion writes no frames before magic is set.
        hdr->version = kSharedVersion;
        hdr->seq = 0;
        hdr->width = hdr->height = 0;
        MemoryBarrier();
        hdr->magic = kSharedMagic;
    }
    return true;
}

bool PreviewSource::Open() {
    if (view_ && !onLocal_) return true;
    const uint64_t now = GetTickCount64();
    if (lastOpenAttempt_ && now - lastOpenAttempt_ < kRetryOpenMs) return view_ != nullptr;
    lastOpenAttempt_ = now;
    // Global\MyCamFrame is the section apps read, so prefer it even after falling back below: once an app
    // opens the camera the companion writes there and the local one goes stale.
    if (OpenNamed(kSharedFrameName, false)) return true;
    return view_ != nullptr || OpenNamed(kSharedFrameNameLocal, true);
}

PreviewSource::Result PreviewSource::Poll(uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
    if (!width || !height || (width & 1) || (height & 1) || !Open()) return Result::NoFrame;
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (hdr->magic != kSharedMagic) return Result::NoFrame;
    // Asking for a frame is what makes the companion keep the phone streaming (FrameWriter::ConsumerActive),
    // so the preview holds the stream up on its own while it is on screen, with the same grace as an app.
    if (writable_) hdr->consumerTick = GetTickCount64();
    const uint64_t tick = hdr->frameTick;
    if (!tick || GetTickCount64() - tick > kStaleMs) return Result::NoFrame;
    if (hdr->seq == lastSeq_ && width == lastW_ && height == lastH_) return Result::Unchanged;

    nv12_.resize(size_t(width) * 2 * height * 2 * 3 / 2);
    const uint32_t ow = width * 2, oh = height * 2;
    // Seqlock read: render straight from shared memory, and retry if the companion wrote meanwhile.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG seq = hdr->seq;
        if (seq & 1) {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        const uint32_t w = hdr->width, h = hdr->height, rotation = hdr->rotation;
        const bool mirror = hdr->mirror != 0, fill = hdr->fill != 0;
        if (!w || !h || w > kMaxWidth || h > kMaxHeight || (w & 1) || (h & 1)) return Result::NoFrame;
        FillBlackNV12(nv12_.data(), ow, ow, oh);
        DrawFittedNV12(FrameData(view_), w, h, rotation % 360, mirror, nv12_.data(), ow, ow, oh, fill);
        MemoryBarrier();
        if (hdr->seq != seq) continue;
        frameW_ = (rotation == 90 || rotation == 270) ? h : w;
        frameH_ = (rotation == 90 || rotation == 270) ? w : h;
        mirror_ = mirror;
        lastSeq_ = seq;
        lastW_ = width;
        lastH_ = height;
        bgra.resize(size_t(width) * height * 4);
        Nv12ToBgraHalf(nv12_.data(), width, height, bgra.data());
        return Result::NewFrame;
    }
    return Result::Unchanged;
}

void PreviewSource::Render(const Nv12Image& image, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
    if (image.empty() || !width || !height) return;
    const uint32_t ow = width * 2, oh = height * 2;
    std::vector<uint8_t> nv12(size_t(ow) * oh * 3 / 2);
    FillBlackNV12(nv12.data(), ow, ow, oh);
    DrawFittedNV12(image.data.data(), image.width, image.height, 0, false, nv12.data(), ow, ow, oh, false);
    bgra.resize(size_t(width) * height * 4);
    Nv12ToBgraHalf(nv12.data(), width, height, bgra.data());
}

} // namespace mycam
