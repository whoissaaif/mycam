#include "frame_writer_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../vcam/frame_transform.h"
#include "log.h"
#include "platform.h"

namespace mycam {

namespace {
std::string g_device;
uint32_t g_width = 0, g_height = 0;
} // namespace

void FrameWriter::Configure(const std::string& device, uint32_t width, uint32_t height) {
    g_device = device;
    g_width = width;
    g_height = height;
}

bool FrameWriter::EnsureOpen() {
    if (fd_ >= 0) return true;
    const uint64_t now = NowMs();
    if (g_device.empty() || (lastAttempt_ && now - lastAttempt_ < 2000)) return false;
    const bool first = lastAttempt_ == 0;
    lastAttempt_ = now;
    std::string why;
    if (Open(g_device, g_width, g_height, &why)) {
        Log("camera: writing %ux%u to %s", g_width, g_height, g_device.c_str());
        return true;
    }
    if (first) Log("camera: %s (retrying every 2 s)", why.c_str());
    return false;
}

FrameWriter::~FrameWriter() { Close(); }

bool FrameWriter::Open(const std::string& device, uint32_t width, uint32_t height, std::string* why) {
    Close();
    if (width == 0 || height == 0 || width % 2 || height % 2) { *why = "size must be even and non-zero"; return false; }
    fd_ = open(device.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) { *why = device + ": " + strerror(errno); return false; }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = width;
    fmt.fmt.pix.sizeimage = width * height * 3 / 2;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M; // BT.601 limited range, what the phone's video decodes to.
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        *why = device + ": not a v4l2loopback output (" + strerror(errno) + ")";
        Close();
        return false;
    }
    if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
        *why = device + ": refused " + std::to_string(width) + "x" + std::to_string(height) + " I420 (already in use?)";
        Close();
        return false;
    }
    width_ = width;
    height_ = height;
    fitted_.resize(size_t(width) * height * 3 / 2);
    out_.resize(fitted_.size());
    return true;
}

void FrameWriter::Close() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

void FrameWriter::Write(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t rotation, bool mirror, bool fill) {
    if (fd_ < 0 && !EnsureOpen()) return;
    if (width == 0 || height == 0) return;
    FillBlackNV12(fitted_.data(), width_, width_, height_);
    DrawFittedNV12(nv12, width, height, rotation, mirror, fitted_.data(), width_, width_, height_, fill);
    Nv12ToI420(fitted_.data(), width_, height_, out_.data());
    // One write() is one frame. A failed or short write (no reader keeping up) just drops this frame.
    if (write(fd_, out_.data(), out_.size()) < 0 && errno != EAGAIN) {
        // The device went away (module unloaded): stop writing until reopened.
        if (errno == ENODEV || errno == EBADF) Close();
    }
}

void FrameWriter::WritePlanes(const uint8_t* y, const uint8_t* uv, ptrdiff_t pitch, uint32_t width, uint32_t height,
                              uint32_t rotation, bool mirror, bool fill) {
    if (pitch == ptrdiff_t(width) && uv == y + size_t(width) * height) {
        Write(y, width, height, rotation, mirror, fill); // Already packed and contiguous (the FFmpeg decoder's output).
        return;
    }
    packed_.resize(size_t(width) * height * 3 / 2);
    for (uint32_t r = 0; r < height; ++r) memcpy(packed_.data() + size_t(r) * width, y + r * pitch, width);
    uint8_t* dstUv = packed_.data() + size_t(width) * height;
    for (uint32_t r = 0; r < height / 2; ++r) memcpy(dstUv + size_t(r) * width, uv + r * pitch, width);
    Write(packed_.data(), width, height, rotation, mirror, fill);
}

} // namespace mycam
