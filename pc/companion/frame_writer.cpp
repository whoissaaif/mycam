#include "frame_writer.h"

#include <string.h>

#include "../common/shared_frame.h"

namespace mycam {

FrameWriter::~FrameWriter() { Close(); }

void FrameWriter::Close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    view_ = nullptr;
    mapping_ = nullptr;
}

bool FrameWriter::EnsureMapping() {
    if (view_) return true;
    uint64_t now = GetTickCount64();
    if (now - lastAttempt_ < 500) return false; // Don't hammer OpenFileMapping while no app uses the camera.
    lastAttempt_ = now;

    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedFrameName);
    if (!mapping_) return false;
    view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSharedBytes);
    if (!view_) {
        Close();
        return false;
    }
    return true;
}

bool FrameWriter::ConsumerActive(uint64_t withinMs) {
    if (!EnsureMapping()) return false;
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (hdr->magic != kSharedMagic) return false;
    uint64_t tick = hdr->consumerTick;
    return tick != 0 && GetTickCount64() - tick < withinMs;
}

void FrameWriter::SetPhoneState(uint32_t state) {
    if (!EnsureMapping()) return;
    static_cast<SharedHeader*>(view_)->phoneState = state;
}

void FrameWriter::Write(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t rotation, bool mirror) {
    if (width > kMaxWidth || height > kMaxHeight || !EnsureMapping()) return;
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (hdr->magic != kSharedMagic) return;

    InterlockedIncrement(&hdr->seq); // Odd: writing.
    hdr->width = width;
    hdr->height = height;
    hdr->rotation = rotation;
    hdr->mirror = mirror ? 1 : 0;
    memcpy(FrameData(view_), nv12, size_t(width) * height * 3 / 2);
    hdr->frameTick = GetTickCount64();
    InterlockedIncrement(&hdr->seq); // Even: stable.
}

} // namespace mycam
