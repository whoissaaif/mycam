#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
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

namespace wifi { class RecordKey; }

// Forgets every phone paired over Wi-Fi (they must pair again with a code). Settings window.
void ForgetPairedPhones();

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
    bool wireless = false;        // The current phone is connected over Wi-Fi (else USB).
    bool wirelessSearch = false;  // "Find phones on Wi-Fi" is on.
    std::wstring phoneName;       // Wi-Fi phones: the name the phone announced.
    std::wstring pairingCode;     // Pairing a Wi-Fi phone: the 6-digit code both screens must show.
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
    // Wireless mode (beta): also look for phones on the local network and connect over Wi-Fi.
    void SetWireless(bool on) { wireless_ = on; }
    // Pause / resume the camera (the phone stores the choice). Needs a connected phone.
    void RequestPause(bool pause) { pendingPause_ = pause ? 1 : 0; }
    // Windows locked / asleep: keep the phone camera off until unlocked. Independent of RequestPause.
    void SetLockPaused(bool locked) { lockPaused_ = locked; }
    // Pictures shown on the MyCam camera while there is no live video. Call before Run().
    void SetStatusImages(Nv12Image paused, Nv12Image waiting) {
        pausedImage_ = std::move(paused);
        waitingImage_ = std::move(waiting);
    }

private:
    void ScanOnce();
    bool LooksLikeAndroid(libusb_device* dev);
    void SwitchToAccessory(libusb_device* dev);
    void RunSession(libusb_device* dev, libusb_context* ctx);
    libusb_device* FindAccessory(libusb_context* ctx);

    // Shared by USB and Wi-Fi sessions (all on the worker thread).
    void BeginSession(bool wireless, const std::string& phoneName);
    bool SessionLoop(const std::function<void()>& pump); // True if a reconnect was asked for.
    void LeaveSession();      // Before the link closes: STOP the camera if the companion is quitting.
    void ResetAfterSession(); // After the link closed.
    void FeedBytes(const uint8_t* data, size_t size);

    // Wireless (Wi-Fi) transport: UDP discovery, then the same protocol over TCP. PROTOCOL.md "Wireless".
    void NetScanOnce();
    bool EnsureUdp();
    void BroadcastProbe();
    // How a Wi-Fi session ended, for when to try that phone again.
    enum class WifiEnd { Ran, Refused, NeedsPairing, Failed };
    WifiEnd RunTcpSession(uintptr_t socket, const std::string& phoneName, bool forcePair);
    WifiEnd Handshake(uintptr_t socket, bool forcePair); // Pairing / paired proof, then sets wifiTx_/wifiRx_.
    bool ReceiveRecords(uintptr_t socket);               // Decrypts what arrived and feeds the parser.
    bool UsbPhoneArrived(); // During a Wi-Fi session: a phone was plugged in (the cable takes over).
    bool SendTcp(const uint8_t* data, size_t size);

    bool SendCommand(uint8_t cmd, uint8_t arg = 0);
    void HandlePacket(uint8_t type, uint8_t flags, int64_t ptsUs, const uint8_t* payload, uint32_t length);
    void OnDecodedFrame(const H264Decoder::Nv12View& frame);
    void Publish();
    void OnInTransfer(libusb_transfer* transfer);
    void LogStats(uint64_t now);
    void SyncLockPaused();
    void WriteStatusFrame(uint64_t now);

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

    std::atomic<bool> wireless_{false};
    bool usbReady_ = false;
    uintptr_t udp_ = ~uintptr_t(0);       // SOCKET for discovery (INVALID_SOCKET when closed).
    uintptr_t tcp_ = ~uintptr_t(0);       // SOCKET of the current Wi-Fi session.
    uint64_t lastProbe_ = 0;
    struct NetPhone { uint16_t port = 0; std::string name; uint64_t lastSeen = 0, retryAfter = 0; bool forcePair = false; };
    // Encrypted Wi-Fi session (PROTOCOL.md "Wireless security"): record keys per direction, received bytes.
    std::shared_ptr<wifi::RecordKey> wifiTx_, wifiRx_;
    std::vector<uint8_t> wifiRxBuf_;
    std::map<uint32_t, NetPhone> netPhones_; // By IPv4 address (network order), from discovery answers.

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
