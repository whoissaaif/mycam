#pragma once
// The settings window's preview tap (docs/redesign.md §8.2). It reads the frame the companion last handed to
// the virtual camera (shared memory Global\MyCamFrame, pc/common/shared_frame.h) and renders it the way
// the virtual camera does: rotated, mirrored and fitted or filled into 16:9. So the preview shows exactly
// what apps receive (live video, or the Paused / animated Waiting picture), and the session loop needs no
// changes. Runs on the UI thread, only while the window is visible.
//
// Polling marks the section's consumerTick, so showing the preview counts as using the camera exactly as
// an app does and the phone streams for the window alone (issue #71). Frame Server's Global section only
// exists while an app has the camera open, so with no app this opens (or creates) the session-local
// section the companion falls back to instead.

#include <windows.h>
#include <stdint.h>

#include <vector>

#include "status_images.h"

namespace mycam {

class PreviewSource {
public:
    ~PreviewSource() { Close(); }

    enum class Result { NoFrame, Unchanged, NewFrame };

    // Renders the latest fresh frame into `bgra` (width x height, both even, BGRA). NoFrame: nothing has
    // been written yet, or the last frame is older than 1.5 s (what the virtual camera treats as stale).
    Result Poll(uint32_t width, uint32_t height, std::vector<uint8_t>& bgra);
    // Size of the last frame read (before fitting), e.g. for the "1080p" chip.
    uint32_t FrameWidth() const { return frameW_; }
    uint32_t FrameHeight() const { return frameH_; }
    bool Mirrored() const { return mirror_; }
    void Close();

    // Renders an NV12 picture (the status images) the same way, letterboxed into 16:9.
    static void Render(const Nv12Image& image, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra);

private:
    bool Open();
    // local: the session-local fallback, which this side may create and initialise; Global\ is only opened.
    bool OpenNamed(const wchar_t* name, bool local);

    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    bool writable_ = false; // False: the section could only be mapped read-only, so no consumerTick.
    bool onLocal_ = false;  // view_ is the session-local section; keep looking for Global\ (an app opened it).
    uint64_t lastOpenAttempt_ = 0;
    LONG lastSeq_ = -1;
    uint32_t lastW_ = 0, lastH_ = 0;
    uint32_t frameW_ = 0, frameH_ = 0;
    bool mirror_ = false;
    std::vector<uint8_t> nv12_; // 2x the output size, box-filtered down in the conversion.
};

} // namespace mycam
