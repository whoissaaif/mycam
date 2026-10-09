#pragma once
#include <atomic>
#include <functional>
#include <stddef.h>
#include <stdint.h>

namespace mycam {

enum class TransportEvent {
    NoDriver,
    Searching,
    Waiting,
    Connected,
    Disconnected,
    Error,
    DriverNeeded,
};

class ITransport {
public:
    using EventCallback = std::function<void(TransportEvent)>;
    using DataCallback = std::function<void(const uint8_t*, size_t)>;

    virtual ~ITransport() = default;

    virtual void SetEventCallback(EventCallback cb) = 0;
    virtual void SetDataCallback(DataCallback cb) = 0;
    virtual bool Connect(std::atomic<bool>& quit) = 0;
    virtual bool Write(const uint8_t* data, size_t size, uint32_t timeoutMs) = 0;
    virtual void Close() = 0;
    virtual void RequestReconnect() = 0;
};

} // namespace mycam
