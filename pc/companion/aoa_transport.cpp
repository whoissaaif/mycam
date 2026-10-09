#include "aoa_transport.h"

#include <windows.h>
#include <libusb.h>
#include <iterator>
#include <string.h>

#include "log.h"
#include "protocol.h"
#include "winusb_bind.h"

namespace mycam {

namespace {

constexpr uint16_t kGoogleVid = 0x18D1;
constexpr int kAoaGetProtocol = 51;
constexpr int kAoaSendString = 52;
constexpr int kAoaStart = 53;

// USB vendors that make Android phones. Used together with interface checks to decide which devices
// are worth asking to switch into accessory mode.
constexpr uint16_t kAndroidVendors[] = {
    0x18D1, 0x04E8, 0x2717, 0x2A70, 0x22B8, 0x1004, 0x12D1, 0x22D9, 0x2D95, 0x0FCE, 0x0BB4, 0x0B05,
    0x17EF, 0x19D2, 0x05C6, 0x0E8D, 0x1782, 0x2A45, 0x1BBB, 0x2E04, 0x0489, 0x2B0E, 0x2D96, 0x1EBF,
};

bool IsAccessoryPid(uint16_t pid) { return pid >= 0x2D00 && pid <= 0x2D05; }

std::string DeviceKey(libusb_device* dev, const libusb_device_descriptor& desc) {
    uint8_t ports[8];
    int n = libusb_get_port_numbers(dev, ports, sizeof(ports));
    std::string key = std::to_string(libusb_get_bus_number(dev));
    for (int i = 0; i < n; ++i) key += "." + std::to_string(ports[i]);
    key += ":" + std::to_string(desc.idVendor) + ":" + std::to_string(desc.idProduct);
    return key;
}

} // namespace

AoaTransport::~AoaTransport() {
    Close();
    if (winusbCtx_) libusb_exit(winusbCtx_);
    winusbCtx_ = nullptr;
    if (ctx_) libusb_exit(ctx_);
    ctx_ = nullptr;
}

void AoaTransport::Emit(TransportEvent event) {
    if (onEvent_) onEvent_(event);
}

bool AoaTransport::EnsureContexts() {
    if (ctx_) return true;
    // UsbDk lets libusb talk to phones while Windows' own MTP/ADB drivers are bound to them.
    // libusb silently falls back to WinUSB without it, which cannot open phones, so check explicitly.
    HMODULE usbdk = LoadLibraryExW(L"UsbDkHelper.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (usbdk) FreeLibrary(usbdk);
    // Switch to UsbDk after init: libusb_init_context applies options before it has probed for UsbDk,
    // so passing LIBUSB_OPTION_USE_USBDK there always fails with LIBUSB_ERROR_NOT_FOUND.
    bool ok = usbdk && libusb_init_context(&ctx_, nullptr, 0) == LIBUSB_SUCCESS;
    if (ok && libusb_set_option(ctx_, LIBUSB_OPTION_USE_USBDK) != LIBUSB_SUCCESS) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
        ok = false;
    }
    Log("usb: UsbDk helper %s, libusb %s", usbdk ? "found" : "MISSING", ok ? "ready" : "FAILED");
    if (!ok) {
        Emit(TransportEvent::NoDriver);
        return false;
    }
    if (libusb_init_context(&winusbCtx_, nullptr, 0) != LIBUSB_SUCCESS) winusbCtx_ = nullptr;
    return true;
}

bool AoaTransport::Connect(std::atomic<bool>& quit) {
    if (!EnsureContexts()) return false;
    Emit(TransportEvent::Searching);

    bool connected = false;
    while (!quit.load() && !connected) {
        ScanOnce(quit, &connected);
        for (int i = 0; i < 10 && !quit.load() && !connected; ++i) {
            Sleep(100);
        }
    }
    return connected;
}

void AoaTransport::ScanOnce(std::atomic<bool>& quit, bool* connected) {
    // Windows has no driver for a phone in accessory mode, and a driverless device makes UsbDk fail to
    // enumerate anything. Ask the UI to bind WinUSB (one UAC prompt per phone; it sticks afterwards).
    if (AccessoryNeedsDriver()) {
        uint64_t now = GetTickCount64();
        if (lastDriverRequest_ == 0 || now - lastDriverRequest_ > 20000) {
            lastDriverRequest_ = now;
            Log("scan: accessory-mode phone has no driver, requesting WinUSB binding");
            Emit(TransportEvent::DriverNeeded);
        }
        Emit(TransportEvent::Waiting);
        return;
    }

    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &list);
    if (count < 0) {
        static uint64_t lastWarn = 0;
        if (GetTickCount64() - lastWarn > 10000) { Log("scan: device list failed: %s", libusb_error_name(int(count))); lastWarn = GetTickCount64(); }
        return;
    }

    libusb_device* accessory = nullptr;
    std::set<std::string> present;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) continue;
        if (desc.idVendor == kGoogleVid && IsAccessoryPid(desc.idProduct)) {
            if (!accessory) accessory = list[i];
            continue;
        }
        std::string key = DeviceKey(list[i], desc);
        present.insert(key);
        // Ask each phone once per plug-in; the set entry goes away when it is unplugged.
        if (!probed_.count(key) && LooksLikeAndroid(list[i])) {
            probed_.insert(key);
            SwitchToAccessory(list[i]);
        }
    }
    for (auto it = probed_.begin(); it != probed_.end();) {
        it = present.count(*it) ? std::next(it) : probed_.erase(it);
    }

    if (accessory) {
        libusb_ref_device(accessory);
        libusb_free_device_list(list, 1);
        // Prefer WinUSB: UsbDk refuses to redirect a device that already has a working driver, and the
        // accessory gets WinUSB bound on first connection (see winusb_bind.cpp).
        libusb_device* viaWinUsb = winusbCtx_ ? FindAccessory(winusbCtx_) : nullptr;
        if (viaWinUsb) {
            RunSession(viaWinUsb, winusbCtx_, quit);
            libusb_unref_device(viaWinUsb);
        } else {
            RunSession(accessory, ctx_, quit);
        }
        libusb_unref_device(accessory);
        *connected = true;
    } else {
        libusb_free_device_list(list, 1);
        Emit(TransportEvent::Searching);
    }
}

bool AoaTransport::LooksLikeAndroid(libusb_device* dev) {
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) return false;
    if (desc.bDeviceClass == LIBUSB_CLASS_HUB) return false;

    bool knownVendor = false;
    for (uint16_t vid : kAndroidVendors) knownVendor |= desc.idVendor == vid;

    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(dev, &config) != LIBUSB_SUCCESS) return false;
    bool phoneInterface = false, excluded = false;
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& itf = config->interface[i];
        for (int a = 0; a < itf.num_altsetting; ++a) {
            const libusb_interface_descriptor& alt = itf.altsetting[a];
            switch (alt.bInterfaceClass) {
            case LIBUSB_CLASS_HID:
            case LIBUSB_CLASS_PRINTER:
            case LIBUSB_CLASS_MASS_STORAGE:
            case LIBUSB_CLASS_HUB:
            case LIBUSB_CLASS_VIDEO:
                excluded = true; // Keyboards, drives, webcams...: never poke these.
                break;
            case LIBUSB_CLASS_IMAGE: // PTP
                phoneInterface = true;
                break;
            case LIBUSB_CLASS_VENDOR_SPEC:
                // ADB (ff/42/01) or Android MTP (ff/ff/00 with bulk in/out + interrupt).
                if ((alt.bInterfaceSubClass == 0x42 && alt.bInterfaceProtocol == 0x01) ||
                    (alt.bInterfaceSubClass == 0xFF && alt.bInterfaceProtocol == 0x00 && alt.bNumEndpoints == 3)) {
                    phoneInterface = true;
                }
                break;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return !excluded && (phoneInterface || knownVendor);
}

void AoaTransport::SwitchToAccessory(libusb_device* dev) {
    libusb_device_handle* h = nullptr;
    libusb_device_descriptor desc = {};
    libusb_get_device_descriptor(dev, &desc);
    int openResult = libusb_open(dev, &h);
    Log("scan: phone-like device %04x:%04x, open -> %s", desc.idVendor, desc.idProduct, libusb_error_name(openResult));
    if (openResult != LIBUSB_SUCCESS) return;

    uint8_t version[2] = {};
    int r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, kAoaGetProtocol, 0, 0,
                                    version, sizeof(version), 1000);
    Log("scan: AOA protocol query -> %d (version %d)", r, version[0] | version[1] << 8);
    if (r == 2 && (version[0] | version[1] << 8) >= 1) {
        const char* strings[] = {proto::kAccessoryManufacturer, proto::kAccessoryModel, proto::kAccessoryDescription,
                                 proto::kAccessoryVersion, proto::kAccessoryUri, proto::kAccessorySerial};
        bool ok = true;
        for (uint16_t i = 0; i < 6 && ok; ++i) {
            ok = libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR, kAoaSendString, 0, i,
                                         (unsigned char*)strings[i], uint16_t(strlen(strings[i]) + 1), 1000) >= 0;
        }
        // The phone disconnects and comes back as a Google accessory device (18d1:2d0x).
        if (ok) libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR, kAoaStart, 0, 0, nullptr, 0, 1000);
    }
    libusb_close(h);
}

libusb_device* AoaTransport::FindAccessory(libusb_context* ctx) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) return nullptr;
    libusb_device* found = nullptr;
    for (ssize_t i = 0; i < count && !found; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == LIBUSB_SUCCESS && desc.idVendor == kGoogleVid &&
            IsAccessoryPid(desc.idProduct)) {
            found = libusb_ref_device(list[i]);
        }
    }
    libusb_free_device_list(list, 1);
    return found;
}

void AoaTransport::RunSession(libusb_device* dev, libusb_context* ctx, std::atomic<bool>& quit) {
    sessionCtx_ = ctx;
    int openResult = libusb_open(dev, &handle_);
    if (openResult != LIBUSB_SUCCESS) {
        static uint64_t lastWarn = 0;
        if (GetTickCount64() - lastWarn > 10000) {
            Log("session: opening accessory failed: %s", libusb_error_name(openResult));
            lastWarn = GetTickCount64();
        }
        handle_ = nullptr;
        return;
    }

    // Interface 0 of the accessory configuration has one bulk IN and one bulk OUT endpoint.
    libusb_config_descriptor* config = nullptr;
    epIn_ = epOut_ = 0;
    if (libusb_get_active_config_descriptor(dev, &config) == LIBUSB_SUCCESS) {
        const libusb_interface_descriptor& alt = config->interface[0].altsetting[0];
        for (int e = 0; e < alt.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = alt.endpoint[e];
            if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) epIn_ = ep.bEndpointAddress;
            else epOut_ = ep.bEndpointAddress;
        }
        libusb_free_config_descriptor(config);
    }
    if (!epIn_ || !epOut_ || libusb_claim_interface(handle_, 0) != LIBUSB_SUCCESS) {
        libusb_close(handle_);
        handle_ = nullptr;
        return;
    }

    Log("session: accessory opened via %s (in 0x%02x, out 0x%02x)", ctx == ctx_ ? "UsbDk" : "WinUSB", epIn_, epOut_);

    // Keep several reads queued with no timeout. Timed-out reads get cancelled, and with UsbDk a
    // cancelled read can drop bytes that were already in flight, which corrupts the stream.
    constexpr int kQueued = 4;
    constexpr int kReadSize = 16384; // Matches the phone's accessory request size.
    std::vector<std::vector<uint8_t>> buffers(kQueued, std::vector<uint8_t>(kReadSize));
    std::vector<libusb_transfer*> transfers;
    sessionEnding_ = false;
    sessionError_ = false;
    inFlight_ = 0;
    for (auto& b : buffers) {
        libusb_transfer* t = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(t, handle_, epIn_, b.data(), kReadSize,
                                  [](libusb_transfer* x) { static_cast<AoaTransport*>(x->user_data)->OnInTransfer(x); },
                                  this, 0);
        if (libusb_submit_transfer(t) == LIBUSB_SUCCESS) ++inFlight_;
        else Log("session: submit failed");
        transfers.push_back(t);
    }
    Emit(TransportEvent::Connected);

    while (!quit.load() && !sessionError_.load()) {
        timeval tv = {0, 100000};
        libusb_handle_events_timeout_completed(sessionCtx_, &tv, nullptr);

        if (reconnect_.exchange(false)) {
            // Forces the phone out of accessory mode; it re-enumerates and we switch it back.
            libusb_reset_device(handle_);
            break;
        }
    }
    Log("session: ending (quit=%d, error=%d)", int(quit.load()), int(sessionError_.load()));

    sessionEnding_ = true;
    for (libusb_transfer* t : transfers) libusb_cancel_transfer(t);
    for (int i = 0; i < 50 && inFlight_ > 0; ++i) {
        timeval tv = {0, 20000};
        libusb_handle_events_timeout_completed(sessionCtx_, &tv, nullptr);
    }
    if (inFlight_ == 0) {
        for (libusb_transfer* t : transfers) libusb_free_transfer(t);
    } // else: leak rather than free transfers libusb still owns.
    libusb_release_interface(handle_, 0);
    libusb_close(handle_);
    handle_ = nullptr;
    Emit(TransportEvent::Disconnected);
}

void AoaTransport::OnInTransfer(libusb_transfer* t) {
    if (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        if (t->actual_length > 0) {
            if (onData_) onData_(t->buffer, size_t(t->actual_length));
        }
        if (!sessionEnding_ && libusb_submit_transfer(t) == LIBUSB_SUCCESS) return;
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        Log("session: read failed (status %d), phone disconnected?", int(t->status));
        sessionError_ = true;
    }
    --inFlight_;
}

bool AoaTransport::Write(const uint8_t* data, size_t size, uint32_t timeoutMs) {
    if (!handle_) return false;
    int sent = 0;
    int r = libusb_bulk_transfer(handle_, epOut_, const_cast<unsigned char*>(data), int(size), &sent, timeoutMs);
    if (r != LIBUSB_SUCCESS || sent != int(size)) {
        Log("usb write failed: %s", libusb_error_name(r));
        return false;
    }
    return true;
}

void AoaTransport::Close() {
    sessionError_ = true;
}

} // namespace mycam
