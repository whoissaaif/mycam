#pragma once
#include <atomic>
#include <stdint.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "transport.h"

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;
struct libusb_transfer;

namespace mycam {

class AoaTransport : public ITransport {
public:
    ~AoaTransport() override;

    void SetEventCallback(EventCallback cb) override { onEvent_ = std::move(cb); }
    void SetDataCallback(DataCallback cb) override { onData_ = std::move(cb); }
    bool Connect(std::atomic<bool>& quit) override;
    bool Write(const uint8_t* data, size_t size, uint32_t timeoutMs) override;
    void Close() override;
    void RequestReconnect() override { reconnect_ = true; }

private:
    bool EnsureContexts();
    void ScanOnce(std::atomic<bool>& quit, bool* connected);
    bool LooksLikeAndroid(libusb_device* dev);
    void SwitchToAccessory(libusb_device* dev);
    void RunSession(libusb_device* dev, libusb_context* ctx, std::atomic<bool>& quit);
    libusb_device* FindAccessory(libusb_context* ctx);
    void OnInTransfer(libusb_transfer* transfer);
    void Emit(TransportEvent event);

    EventCallback onEvent_;
    DataCallback onData_;
    uint64_t lastDriverRequest_ = 0;
    std::atomic<bool> reconnect_{false};

    libusb_context* ctx_ = nullptr;       // UsbDk backend: switches phones into accessory mode.
    libusb_context* winusbCtx_ = nullptr; // WinUSB backend: streams from accessory-mode phones (WinUSB-bound).
    libusb_context* sessionCtx_ = nullptr;
    std::set<std::string> probed_;

    libusb_device_handle* handle_ = nullptr;
    unsigned char epIn_ = 0, epOut_ = 0;
    int inFlight_ = 0;
    bool sessionEnding_ = false;
    std::atomic<bool> sessionError_{false};
};

} // namespace mycam
