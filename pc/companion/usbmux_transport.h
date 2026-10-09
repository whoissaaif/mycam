#pragma once
#include <atomic>
#include <stdint.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "transport.h"
#include "usbmux_proto.h"

namespace mycam {

class UsbmuxTransport : public ITransport {
public:
    using SocketHandle = uintptr_t;

    ~UsbmuxTransport() override;

    void SetEventCallback(EventCallback cb) override { onEvent_ = std::move(cb); }
    void SetDataCallback(DataCallback cb) override { onData_ = std::move(cb); }
    bool Connect(std::atomic<bool>& quit) override;
    bool Write(const uint8_t* data, size_t size, uint32_t timeoutMs) override;
    void Close() override;
    void RequestReconnect() override;

private:
    bool EnsureWinsock();
    SocketHandle OpenUsbmuxSocket();
    bool SendAll(SocketHandle s, const uint8_t* data, size_t size);
    bool RecvPackets(SocketHandle s, usbmux::PacketReader* reader, const usbmux::PacketReader::Callback& cb);
    bool SendPacket(SocketHandle s, const usbmux::Bytes& packet);
    bool ReadResult(SocketHandle s, uint32_t tag, uint32_t* number);
    SocketHandle ConnectToDevice(uint32_t deviceId);
    void StartTunnel(SocketHandle s, uint32_t deviceId);
    void ReaderLoop(SocketHandle s);
    void CloseTunnel(bool joinReader);
    void Emit(TransportEvent event);

    EventCallback onEvent_;
    DataCallback onData_;
    std::atomic<bool> reconnect_{false};
    uint32_t nextTag_ = 1;
    bool wsaReady_ = false;
    bool loggedUnavailable_ = false;

    std::mutex socketLock_;
    SocketHandle tunnel_ = UINTPTR_MAX;
    uint32_t activeDeviceId_ = 0;
    std::thread readerThread_;
};

} // namespace mycam
