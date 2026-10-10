#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "decoder.h"
#include "packet_parser.h"
#include "frame_writer.h"
#include "protocol.h"
#include "status_images.h"

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;
struct libusb_transfer;

namespace mycam {

enum class LinkState {
    NoDriver,    // UsbDk is not installed; we cannot talk to phones.
    Searching,   // No phone found.
    Waiting,     // Phone is in accessory mode but the MyCam app has not answered yet.
    Idle,        // Connected; camera off because no PC app is using the webcam.
    Streaming,   // Frames flowing.
    PhoneError,  // The phone reported a camera problem.
    Paused,      // The user paused the camera (on the phone or the PC). Camera is off.
};

struct LinkStatus {
    LinkState state = LinkState::Searching;
    int facing = 0;          // proto::Facing
    uint32_t width = 0, height = 0;
    bool lockPaused = false; // Windows is locked or asleep: MyCam keeps the phone camera off.
    proto::CameraInfo camera; // v3: phone video settings + what its camera supports (valid once received).
};

// Owns all USB traffic. Run() blocks on the calling (worker) thread until Quit() is called.
class PhoneLink {
public:
    using StatusCallback = std::function<void(const LinkStatus&)>;

    // onNeedDriver: a phone in accessory mode has no driver yet; the UI should run the elevated binder.
    PhoneLink(StatusCallback onStatus, std::function<void()> onNeedDriver)
        : onStatus_(std::move(onStatus)), onNeedDriver_(std::move(onNeedDriver)) {}

    void Run();
    void Quit() { quit_ = true; }

    // Thread-safe controls from the UI thread.
    void RequestFacing(int facing) { pendingFacing_ = facing; }
    void SetMirror(bool mirror) { mirror_ = mirror; }
    void SetFill(bool fill) { fill_ = fill; }
    // Queues a v3 camera command (quality, fps, zoom, exposure, torch, focus) for the phone.
    void RequestCommand(uint8_t cmd, uint8_t arg) {
        std::lock_guard<std::mutex> lock(commandLock_);
        commands_.push_back({cmd, arg});
    }
    void RequestReconnect() { reconnect_ = true; }
    // Pause / resume the camera (the phone stores the choice). Needs a connected phone.
    void RequestPause(bool pause) { pendingPause_ = pause ? 1 : 0; }
    // Windows locked / asleep: keep the phone camera off until unlocked. Independent of RequestPause.
    void SetLockPaused(bool locked) { lockPaused_ = locked; }
    // Pictures shown on the MyCam camera while there is no live video. Call before Run().
    void SetStatusImages(Nv12Image paused, Nv12Image waiting) {
        pausedImage_ = std::move(paused);
        waitingImage_ = std::move(waiting);
        waitingFrame_ = waitingImage_;
    }

private:
    void ScanOnce();
    bool LooksLikeAndroid(libusb_device* dev);
    void SwitchToAccessory(libusb_device* dev);
    void RunSession(libusb_device* dev, libusb_context* ctx);
    libusb_device* FindAccessory(libusb_context* ctx);

    bool SendCommand(uint8_t cmd, uint8_t arg = 0);
    void HandlePacket(uint8_t type, uint8_t flags, int64_t ptsUs, const uint8_t* payload, uint32_t length);
    void OnDecodedFrame(const H264Decoder::Nv12View& frame);
    void Publish();
    void OnInTransfer(libusb_transfer* transfer);
    void LogStats(uint64_t now);
    void SyncLockPaused();
    void WriteStatusFrame(uint64_t now);
    // How long the loops may wait before the next status frame is due (shorter while the marquee runs).
    uint32_t StatusWaitMs() const { return marqueeActive_ ? 66 : 100; }

    StatusCallback onStatus_;
    std::function<void()> onNeedDriver_;
    uint64_t lastDriverRequest_ = 0;
    std::atomic<bool> quit_{false};
    std::atomic<int> pendingFacing_{-1};
    std::atomic<bool> mirror_{false};
    std::atomic<bool> fill_{false};
    std::mutex commandLock_;
    std::vector<std::pair<uint8_t, uint8_t>> commands_;
    std::atomic<bool> reconnect_{false};
    std::atomic<int> pendingPause_{-1};
    std::atomic<bool> lockPaused_{false};
    Nv12Image pausedImage_, waitingImage_;
    uint64_t lastStatusFrame_ = 0;
    Nv12Image waitingFrame_;     // waitingImage_ with the marquee drawn in (worker thread only).
    bool marqueeActive_ = false; // The animated waiting picture is on screen: write it at ~15 fps.

    libusb_context* ctx_ = nullptr;       // UsbDk backend: switches phones into accessory mode.
    libusb_context* winusbCtx_ = nullptr; // WinUSB backend: streams from accessory-mode phones (WinUSB-bound).
    libusb_context* sessionCtx_ = nullptr;
    std::set<std::string> probed_;

    // Session state (worker thread only).
    libusb_device_handle* handle_ = nullptr;
    unsigned char epIn_ = 0, epOut_ = 0;
    PacketParser parser_;
    bool gotHello_ = false;
    bool startSent_ = false;
    bool phoneStreaming_ = false;
    bool phonePaused_ = false;
    bool waitKeyFrame_ = true;
    std::vector<uint8_t> csd_;
    std::vector<uint8_t> scratch_;
    uint32_t configWidth_ = 0, configHeight_ = 0;
    int sensorOrientation_ = 0;
    int deviceRotation_ = 0;
    uint64_t lastKeyRequest_ = 0;
    uint64_t lastFrameTick_ = 0;

    // Queued bulk-IN reads (never time out, so no data is lost to cancellation).
    int inFlight_ = 0;
    bool sessionEnding_ = false;
    bool sessionError_ = false;
    bool keyFrameWanted_ = false;

    // Diagnostics, logged every few seconds.
    uint64_t statBytes_ = 0, statPackets_ = 0, statFrames_ = 0, statDecoded_ = 0, statDecodeErrors_ = 0;
    double statDecodeMs_ = 0; // Total decode time in the current stats window (latency measurement).
    uint64_t lastStats_ = 0;

    H264Decoder decoder_;
    FrameWriter writer_;
    LinkStatus status_;
};

} // namespace mycam
