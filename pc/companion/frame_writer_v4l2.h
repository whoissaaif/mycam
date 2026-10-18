#pragma once
// The Linux FrameWriter: the same Write/WritePlanes as frame_writer.h (Windows shared memory), into a
// v4l2loopback device (/dev/videoN) instead. Apps (browsers, OBS, Zoom) read it as a webcam.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace mycam {

class FrameWriter {
public:
    FrameWriter() = default;
    FrameWriter(const FrameWriter&) = delete;
    FrameWriter& operator=(const FrameWriter&) = delete;
    ~FrameWriter();

    // Opens the device and fixes the camera's format: I420 at width x height (even). The size never changes
    // while the device is open, since apps don't follow a format change mid-stream. Phone frames of any size or
    // orientation are fitted into it. False (with *why) if the device can't be opened or refuses the format.
    bool Open(const std::string& device, uint32_t width, uint32_t height, std::string* why);
    void Close();
    bool IsOpen() const { return fd_ >= 0; }

    // ponytail: no reader detection yet (v4l2loopback has no reader count), so the phone camera stays on while
    // the device is open; add it when the daemon needs to idle the phone like Windows does.
    bool ConsumerActive(uint64_t /*withinMs*/) const { return IsOpen(); }

    void Write(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t rotation, bool mirror, bool fill = false);
    // Same, from separate Y and UV planes with a row pitch (decoder output).
    void WritePlanes(const uint8_t* y, const uint8_t* uv, ptrdiff_t pitch, uint32_t width, uint32_t height,
                     uint32_t rotation, bool mirror, bool fill);

private:
    int fd_ = -1;
    uint32_t width_ = 0, height_ = 0;
    std::vector<uint8_t> packed_; // Decoder planes repacked tightly (DrawFittedNV12 takes packed NV12).
    std::vector<uint8_t> fitted_; // The camera-sized NV12 frame.
    std::vector<uint8_t> out_;    // ...as I420, what goes to the device.
};

} // namespace mycam
