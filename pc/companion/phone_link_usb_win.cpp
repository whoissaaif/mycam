// The USB backend on Windows: UsbDk switches phones into accessory mode; once WinUSB is bound to the
// accessory (winusb_bind.cpp, via the SYSTEM scheduled task), a second libusb context streams through it.

#include <windows.h>

#include <libusb.h>

#include <mutex>
#include <set>
#include <string>

#include "log.h"
#include "phone_link.h"
#include "winusb_bind.h"

namespace mycam {

namespace {

// libusb's own warnings and errors (e.g. "Redirector startup failed" from UsbDk, or "could not open device ...
// interface N: Access is denied") go to mycam.log, each distinct message once: libusb repeats them every scan.
void LIBUSB_CALL LogLibusb(libusb_context*, enum libusb_log_level, const char* message) {
    static std::mutex lock;
    static std::set<std::string> seen;
    std::string text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    std::lock_guard<std::mutex> guard(lock);
    if (text.empty() || seen.size() >= 200 || !seen.insert(text).second) return;
    Log("libusb: %s", text.c_str());
}

void EnableLibusbLog(libusb_context* ctx) {
    libusb_set_log_cb(ctx, LogLibusb, LIBUSB_LOG_CB_CONTEXT);
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
}

} // namespace

bool PhoneLink::InitUsb() {
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
    if (ok) EnableLibusbLog(ctx_);
    if (ok && libusb_init_context(&winusbCtx_, nullptr, 0) != LIBUSB_SUCCESS) winusbCtx_ = nullptr;
    if (winusbCtx_) EnableLibusbLog(winusbCtx_);
    return ok;
}

void PhoneLink::ExitUsb() {
    if (winusbCtx_) libusb_exit(winusbCtx_);
    winusbCtx_ = nullptr;
    if (ctx_) libusb_exit(ctx_);
    ctx_ = nullptr;
}

bool PhoneLink::UsbNeedsDriver() {
    std::string detail;
    bool needs = AccessoryNeedsDriver(&detail);
    static std::string lastDetail; // Say which node and why once per change: this runs every scan.
    if (needs && detail != lastDetail) Log("usb: accessory needs WinUSB: %s", detail.c_str());
    lastDetail = needs ? detail : std::string();
    return needs;
}

bool PhoneLink::RunAccessory(libusb_device* accessory, bool checkOnly) {
    // Prefer WinUSB: UsbDk refuses to redirect a device that already has a working driver, and the
    // accessory gets WinUSB bound on first connection (see winusb_bind.cpp).
    libusb_device* viaWinUsb = winusbCtx_ ? FindAccessory(winusbCtx_) : nullptr;
    if (!viaWinUsb) return RunSession(accessory, ctx_, checkOnly);
    const bool opened = RunSession(viaWinUsb, winusbCtx_, checkOnly);
    libusb_unref_device(viaWinUsb);
    return opened;
}

const char* PhoneLink::BackendName(libusb_context* ctx) const { return ctx == ctx_ ? "UsbDk" : "WinUSB"; }

} // namespace mycam
