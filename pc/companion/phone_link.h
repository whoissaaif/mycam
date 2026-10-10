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
#include "discovery.h"
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

// How long the phone gives its user to answer a pairing request (the PC waits a little longer, see
// kUserTimeoutMs in phone_link_wifi.cpp). The pairing dialog counts this down.
constexpr uint64_t kPairingAnswerMs = 60000;

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
    // "Scan for phones": a one-shot look for phones on the Wi-Fi, and what answered.
    bool scanning = false;                 // A scan window is running.
    uint32_t scansDone = 0;                // Scans finished since the companion started (0: never scanned).
    std::vector<NearbyPhone> nearbyPhones; // At most NearbyList::kMax, in the order they answered.
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
    // "Scan for phones": broadcast a probe now and list every phone that answers for kScanWindowMs, even
    // while "Find phones on Wi-Fi" is off (the setting doesn't change, and nothing connects by itself then).
    void ScanForPhones() { scanRequest_ = true; }
    // Runs one Wi-Fi session to a phone from the scan list (pairing with a code if it's new), even while
    // "Find phones on Wi-Fi" is off. A plugged-in phone still wins. ipv4 in network order.
    void ConnectTo(uint32_t ipv4) { connectRequest_ = ipv4; }
    static constexpr uint64_t kScanWindowMs = 6000;
    // Abandons a Wi-Fi pairing that is waiting for the phone's answer: the PC closes the socket (no
    // protocol message), and that phone isn't asked again for a while. No effect when not pairing.
    void CancelPairing() { cancelPairing_ = true; }
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
    // Discovery without connecting: starts a requested scan, probes while one runs, reads the answers and
    // ends the scan window. Safe inside a session (the SessionLoop calls it).
    void PollDiscovery(uint64_t now);
    void ReadAnswers(uint64_t now);
    void RefreshNearby();            // Paired / connected marks, then Publish() if the list changed.
    bool WifiAllowed() const { return wireless_ || manualSession_; } // A Wi-Fi session may go on.
    void RunManualConnect(uint32_t ip);
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

    std::atomic<bool> wireless_{false};
    std::atomic<bool> cancelPairing_{false};
    bool usbReady_ = false;
    uintptr_t udp_ = ~uintptr_t(0);       // SOCKET for discovery (INVALID_SOCKET when closed).
    uintptr_t tcp_ = ~uintptr_t(0);       // SOCKET of the current Wi-Fi session.
    uint64_t lastProbe_ = 0;
    struct NetPhone { uint16_t port = 0; std::string name; uint64_t lastSeen = 0, retryAfter = 0; bool forcePair = false; };
    // Encrypted Wi-Fi session (PROTOCOL.md "Wireless security"): record keys per direction, received bytes.
    std::shared_ptr<wifi::RecordKey> wifiTx_, wifiRx_;
    std::vector<uint8_t> wifiRxBuf_;
    std::map<uint32_t, NetPhone> netPhones_; // By IPv4 address (network order), from discovery answers.
    std::atomic<bool> scanRequest_{false};
    std::atomic<uint32_t> connectRequest_{0};
    bool manualSession_ = false;  // The current Wi-Fi session was asked for with Connect (search may be off).
    uint64_t scanUntil_ = 0;      // End of the running scan window (0: none).
    uint32_t scansDone_ = 0;
    uint32_t sessionIp_ = 0;      // Peer of the current Wi-Fi session (0: none).
    bool nearbyDirty_ = false;    // nearby_ changed since the last Publish().
    NearbyList nearby_;           // What the window lists.

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
