#include "phone_link.h"

#include <windows.h>

#include "../common/shared_frame.h"
#include "aoa_transport.h"
#include "log.h"
#include "protocol.h"
#include "usbmux_transport.h"

namespace mycam {

namespace {

constexpr uint64_t kConsumerWindowMs = 2000;  // App counts as "using the camera" for this long after a request.
constexpr uint64_t kStopGraceMs = 4000;       // Keep the phone camera on briefly between app sessions.

} // namespace

void PhoneLink::RequestReconnect() {
    for (auto& transport : transports_) transport->RequestReconnect();
}

void PhoneLink::Run() {
    transports_.push_back(std::make_unique<AoaTransport>());
    transports_.push_back(std::make_unique<UsbmuxTransport>());
    for (auto& transport : transports_) {
        ITransport* t = transport.get();
        t->SetEventCallback([this, t](TransportEvent event) { OnTransportEvent(t, event); });
        t->SetDataCallback([this, t](const uint8_t* data, size_t size) { OnTransportData(t, data, size); });
        transportThreads_.emplace_back([this, t] { TransportWorker(t); });
    }
    status_.state = LinkState::Searching;
    Publish();

    while (!quit_) {
        {
            std::lock_guard<std::mutex> lock(sessionLock_);
            SessionTick(GetTickCount64());
        }
        Sleep(100);
    }

    {
        std::lock_guard<std::mutex> lock(sessionLock_);
        // Companion exiting: turn the phone camera off rather than leaving it blocked on a full pipe.
        if (activeTransport_.load() && startSent_) SendCommand(proto::kCmdStop);
    }
    for (auto& transport : transports_) transport->Close();
    activeCv_.notify_all();
    for (auto& thread : transportThreads_) {
        if (thread.joinable()) thread.join();
    }
}

void PhoneLink::TransportWorker(ITransport* transport) {
    while (!quit_) {
        {
            std::unique_lock<std::mutex> lock(activeMutex_);
            activeCv_.wait(lock, [this] { return quit_ || activeTransport_.load() == nullptr; });
        }
        if (quit_) break;
        if (!transport->Connect(quit_)) break;
    }
}

void PhoneLink::OnTransportEvent(ITransport* transport, TransportEvent event) {
    switch (event) {
    case TransportEvent::NoDriver:
        {
            std::lock_guard<std::mutex> lock(sessionLock_);
            status_.state = LinkState::NoDriver;
            Publish();
        }
        break;
    case TransportEvent::Searching:
        if (activeTransport_.load() == nullptr) {
            std::lock_guard<std::mutex> lock(sessionLock_);
            if (status_.state != LinkState::Searching) {
                status_ = LinkStatus{};
                Publish();
            }
        }
        break;
    case TransportEvent::Waiting:
        if (activeTransport_.load() == nullptr) {
            std::lock_guard<std::mutex> lock(sessionLock_);
            if (status_.state != LinkState::Waiting) {
                status_ = LinkStatus{};
                status_.state = LinkState::Waiting;
                Publish();
            }
        }
        break;
    case TransportEvent::Connected:
        {
            ITransport* expected = nullptr;
            if (activeTransport_.compare_exchange_strong(expected, transport)) {
                std::lock_guard<std::mutex> lock(sessionLock_);
                BeginSession(transport);
            } else {
                transport->Close();
            }
        }
        break;
    case TransportEvent::Disconnected:
    case TransportEvent::Error:
        EndSession(transport);
        activeCv_.notify_all();
        break;
    case TransportEvent::DriverNeeded:
        if (onNeedDriver_) onNeedDriver_();
        break;
    }
}

void PhoneLink::OnTransportData(ITransport* transport, const uint8_t* data, size_t size) {
    if (activeTransport_.load() != transport) return;
    std::lock_guard<std::mutex> lock(sessionLock_);
    statBytes_ += size;
    parser_.Feed(data, size, [this](const Packet& p) {
        ++statPackets_;
        HandlePacket(p.type, p.flags, p.ptsUs, p.payload, p.length);
    });
}

void PhoneLink::BeginSession(ITransport*) {
    parser_.Reset();
    gotHello_ = startSent_ = phoneStreaming_ = false;
    phonePaused_ = false;
    waitKeyFrame_ = true;
    keyFrameWanted_ = false;
    csd_.clear();
    decoder_.Reset();
    status_ = LinkStatus{};
    status_.state = LinkState::Waiting;
    writer_.SetPhoneState(kPhoneWaiting);
    Publish();

    statBytes_ = statPackets_ = statFrames_ = statDecoded_ = statDecodeErrors_ = 0;
    SendCommand(proto::kCmdHello);
    lastHello_ = GetTickCount64();
    lastConsumer_ = 0;
    lastStart_ = 0;
    lastStop_ = 0;
    lastStats_ = lastHello_;
}

void PhoneLink::EndSession(ITransport* transport) {
    ITransport* expected = transport;
    if (!activeTransport_.compare_exchange_strong(expected, nullptr)) return;

    std::lock_guard<std::mutex> lock(sessionLock_);
    decoder_.Reset();
    phonePaused_ = phoneStreaming_ = false;
    writer_.SetPhoneState(kPhoneNone);
    status_ = LinkStatus{};
    Publish();
}

void PhoneLink::SessionTick(uint64_t now) {
    LogStats(now);
    SyncLockPaused();
    WriteStatusFrame(now);
    if (!activeTransport_.load()) return;
    if (!gotHello_) {
        if (now - lastHello_ > 2000) {
            SendCommand(proto::kCmdHello);
            lastHello_ = now;
        }
        return;
    }

    if (keyFrameWanted_) {
        keyFrameWanted_ = false;
        SendCommand(proto::kCmdKeyFrame);
    }
    int facing = pendingFacing_.exchange(-1);
    if (facing >= 0) SendCommand(proto::kCmdSetFacing, uint8_t(facing));
    {
        std::vector<std::pair<uint8_t, uint8_t>> queued;
        {
            std::lock_guard<std::mutex> lock(commandLock_);
            queued.swap(commands_);
        }
        for (auto& c : queued) SendCommand(c.first, c.second);
    }
    int pause = pendingPause_.exchange(-1);
    if (pause >= 0) {
        Log("session: user %s the camera from the PC", pause ? "paused" : "resumed");
        SendCommand(pause ? proto::kCmdPause : proto::kCmdResume);
    }

    if (writer_.ConsumerActive(kConsumerWindowMs)) lastConsumer_ = now;
    // Video is wanted only if an app is using the camera and nothing has paused it.
    bool wanted = lastConsumer_ && now - lastConsumer_ < kStopGraceMs && !phonePaused_ && !lockPaused_;
    if (wanted && (!startSent_ || (!phoneStreaming_ && now - lastStart_ > 3000))) {
        // (Re)start the phone camera; repeated if the phone reported an error or never started.
        Log("session: app is using the camera -> START");
        if (!SendCommand(proto::kCmdStart)) {
            ITransport* transport = activeTransport_.load();
            if (transport) transport->Close();
            return;
        }
        startSent_ = true;
        lastStart_ = now;
        waitKeyFrame_ = true;
    } else if (!wanted && (startSent_ || (phoneStreaming_ && now - lastStop_ > 3000))) {
        // Also stops a phone that is streaming without being asked, e.g. after the companion crashed
        // or was killed mid-stream and the phone kept its camera on.
        Log("session: camera not wanted (no app, paused or Windows locked) -> STOP");
        if (!SendCommand(proto::kCmdStop)) {
            ITransport* transport = activeTransport_.load();
            if (transport) transport->Close();
            return;
        }
        startSent_ = false;
        lastStop_ = now;
    }
}

void PhoneLink::LogStats(uint64_t now) {
    if (now - lastStats_ < 3000) return;
    double secs = (now - lastStats_) / 1000.0;
    if (statBytes_ || startSent_) {
        Log("stats: %.0f KB/s, %llu packets, %llu video frames in, %llu decoded, %llu decode errors, "
            "decode+copy avg %.1f ms, app using camera=%d",
            statBytes_ / 1024.0 / secs, statPackets_, statFrames_, statDecoded_, statDecodeErrors_,
            statFrames_ ? statDecodeMs_ / statFrames_ : 0.0, int(writer_.ConsumerActive(kConsumerWindowMs)));
    }
    statDecodeMs_ = 0;
    statBytes_ = statPackets_ = statFrames_ = statDecoded_ = statDecodeErrors_ = 0;
    lastStats_ = now;
}

bool PhoneLink::SendCommand(uint8_t cmd, uint8_t arg) {
    ITransport* transport = activeTransport_.load();
    if (!transport) return false;
    uint8_t packet[proto::kCommandSize];
    proto::MakeCommand(packet, proto::Command(cmd), arg);
    if (!transport->Write(packet, sizeof(packet), 1000)) {
        Log("send cmd %d failed", int(cmd));
        return false;
    }
    return true;
}

void PhoneLink::HandlePacket(uint8_t type, uint8_t flags, int64_t ptsUs, const uint8_t* payload, uint32_t length) {
    switch (type) {
    case proto::kHello:
        gotHello_ = true;
        Log("phone: hello (protocol %d)", length >= 2 ? int(proto::ReadU16(payload)) : -1);
        if (status_.state == LinkState::Waiting) status_.state = LinkState::Idle;
        writer_.SetPhoneState(kPhoneIdle);
        Publish();
        break;

    case proto::kConfig: {
        if (length < 7) return;
        uint32_t w = proto::ReadU16(payload), h = proto::ReadU16(payload + 2);
        sensorOrientation_ = proto::ReadU16(payload + 4);
        status_.facing = payload[6];
        csd_.assign(payload + 7, payload + length);
        if (!decoder_.Ready() || w != configWidth_ || h != configHeight_) {
            configWidth_ = w;
            configHeight_ = h;
            HRESULT hr = decoder_.Init(w, h, [this](const H264Decoder::Nv12View& f) { OnDecodedFrame(f); });
            Log("decoder: init %ux%u -> 0x%08X", w, h, unsigned(hr));
        }
        Log("phone: config %ux%u sensor=%d facing=%d csd=%u bytes", w, h, sensorOrientation_, int(payload[6]), unsigned(csd_.size()));
        waitKeyFrame_ = true;
        status_.width = w;
        status_.height = h;
        Publish();
        break;
    }

    case proto::kFrame: {
        if (phonePaused_) return; // Late frames after a pause must never reach the camera.
        ++statFrames_;
        bool key = flags & proto::kFlagKeyFrame;
        if (!phoneStreaming_) {
            // Frames prove the phone is streaming even if its state/config messages never arrived.
            phoneStreaming_ = true;
            status_.state = LinkState::Streaming;
            writer_.SetPhoneState(kPhoneStreaming);
            Publish();
        }
        if (!decoder_.Ready()) {
            // No config received (some encoders only put SPS/PPS inside key frames). The decoder reads
            // the real size from the SPS and reports a format change, so the size here is only a hint.
            uint32_t w = configWidth_ ? configWidth_ : 1920, h = configHeight_ ? configHeight_ : 1080;
            HRESULT hr = decoder_.Init(w, h, [this](const H264Decoder::Nv12View& f) { OnDecodedFrame(f); });
            Log("decoder: init without config (%ux%u hint) -> 0x%08X", w, h, unsigned(hr));
            if (FAILED(hr)) return;
            waitKeyFrame_ = true;
        }
        uint64_t now = GetTickCount64();
        if (waitKeyFrame_ && !key) {
            if (now - lastKeyRequest_ > 1000) {
                keyFrameWanted_ = true; // Sent from the session loop: blocking sends fail inside USB callbacks.
                lastKeyRequest_ = now;
            }
            return;
        }
        HRESULT hr;
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceCounter(&t0);
        if (key && !csd_.empty()) {
            // Always hand SPS/PPS to the decoder with a key frame so it can (re)start cleanly.
            scratch_.assign(csd_.begin(), csd_.end());
            scratch_.insert(scratch_.end(), payload, payload + length);
            hr = decoder_.Decode(scratch_.data(), scratch_.size(), ptsUs);
        } else {
            hr = decoder_.Decode(payload, length, ptsUs);
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        statDecodeMs_ += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart; // Decode + copy to shared memory.
        if (FAILED(hr)) {
            if (statDecodeErrors_++ < 3) Log("decoder: frame (key=%d, %u bytes) failed 0x%08X", int(key), length, unsigned(hr));
            waitKeyFrame_ = true;
        } else if (key) {
            waitKeyFrame_ = false;
        }
        break;
    }

    case proto::kCamera:
        if (proto::ParseCameraInfo(payload, length, &status_.camera)) {
            Log("phone: camera %ux%u @ %u fps, zoom %.2f (%.2f-%.2f), ev %d, flags 0x%02x", status_.camera.width,
                status_.camera.height, status_.camera.actualFps, status_.camera.zoomX100 / 100.0,
                status_.camera.zoomMinX100 / 100.0, status_.camera.zoomMaxX100 / 100.0, status_.camera.ev,
                status_.camera.flags);
            Publish();
        }
        break;

    case proto::kLog:
        Log("phone says: %.*s", int(length), reinterpret_cast<const char*>(payload));
        break;

    case proto::kOrient:
        if (length >= 2) deviceRotation_ = proto::ReadU16(payload) % 360;
        break;

    case proto::kState:
        if (length < 1) return;
        Log("phone: state %d", int(payload[0]));
        phoneStreaming_ = payload[0] == proto::kStateStreaming;
        phonePaused_ = payload[0] == proto::kStatePaused;
        if (length >= 2) status_.facing = payload[1];
        status_.state = payload[0] == proto::kStateStreaming ? LinkState::Streaming
                      : payload[0] == proto::kStateError     ? LinkState::PhoneError
                      : payload[0] == proto::kStatePaused    ? LinkState::Paused
                                                             : LinkState::Idle;
        if (!phoneStreaming_) status_.width = status_.height = 0;
        writer_.SetPhoneState(phoneStreaming_ ? kPhoneStreaming : kPhoneIdle);
        Publish();
        break;
    }
}

void PhoneLink::OnDecodedFrame(const H264Decoder::Nv12View& frame) {
    // Camera2's JPEG-orientation rule: how far to rotate the sensor image clockwise to look upright.
    int rotation = status_.facing == proto::kFacingFront ? (sensorOrientation_ - deviceRotation_ + 360) % 360
                                                         : (sensorOrientation_ + deviceRotation_) % 360;
    rotation = (rotation + 45) / 90 * 90 % 360;
    ++statDecoded_;
    if (phonePaused_ || lockPaused_) return; // Privacy: nothing reaches the camera while paused or locked.
    writer_.WritePlanes(frame.y, frame.uv, frame.pitch, frame.width, frame.height, uint32_t(rotation), mirror_, fill_);
    lastFrameTick_ = GetTickCount64();
}

void PhoneLink::Publish() {
    status_.lockPaused = lockPaused_;
    if (onStatus_) onStatus_(status_);
}

void PhoneLink::SyncLockPaused() {
    if (status_.lockPaused != lockPaused_) {
        Log("Windows %s: camera %s", lockPaused_ ? "locked" : "unlocked", lockPaused_ ? "paused" : "may resume");
        Publish();
    }
}

// While there is no live video, keep the MyCam camera showing a picture instead of black (the camera
// treats frames older than 1.5 s as stale, so refresh a few times a second).
void PhoneLink::WriteStatusFrame(uint64_t now) {
    if (now - lastStatusFrame_ < 400) return;
    const bool live = phoneStreaming_ && !phonePaused_ && !lockPaused_ && lastFrameTick_ && now - lastFrameTick_ < 1000;
    if (live) return;
    const Nv12Image& image = (phonePaused_ || lockPaused_) ? pausedImage_ : waitingImage_;
    if (image.empty()) return;
    lastStatusFrame_ = now;
    writer_.Write(image.data.data(), image.width, image.height, 0, false);
}

} // namespace mycam
