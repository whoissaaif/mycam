#pragma once

#include <string>
#include <vector>

namespace mycam {

// --- Pure rules (unit-tested in pc/tests) -------------------------------------------------------------

// What a device node is to MyCam, judged from its hardware IDs (SPDRP_HARDWAREID, plus the device-ID part of
// its instance ID) and its parent's.
enum class AccessoryKind {
    None,      // Anything else: never touched.
    Device,    // A phone in plain accessory mode (18d1:2d00): the whole device is the accessory.
    Interface, // Interface 0 of an accessory composite (18d1:2d01/2d04/2d05: accessory + adb and/or audio).
};
AccessoryKind ClassifyAccessory(const std::vector<std::wstring>& ids, const std::vector<std::wstring>& parentIds);

// The driver state of an accessory node, from what Windows reports about it.
struct AccessoryDriverInfo {
    bool hasProblem = false;      // DN_HAS_PROBLEM
    unsigned long problem = 0;    // CM_PROB_*
    std::wstring service;         // SPDRP_SERVICE ("" when no driver)
    std::wstring classGuid;       // The node's setup class, "{...}"
    std::wstring matchingId;      // The ID its driver was picked by (driver key "MatchingDeviceId")
    bool hasInterfaceGuid = false; // "DeviceInterfaceGUID(s)" in its Device Parameters key
};
enum class AccessoryDriver {
    Ready,          // WinUSB (or libusbK/libusb0), reachable by libusb.
    Missing,        // No driver: Windows has none for the accessory interface.
    WrongClass,     // A class driver that can't drive it (WPD/MTP, still image), picked by a compatible ID.
    NoInterfaceGuid,// WinUSB on a composite child with no device interface GUID: libusb can't find it.
    Foreign,        // Some other driver someone installed on purpose: left alone.
};
AccessoryDriver ClassifyAccessoryDriver(AccessoryKind kind, const AccessoryDriverInfo& info);
bool NeedsWinUsbBinding(AccessoryDriver state);
const char* AccessoryDriverName(AccessoryDriver state);

// --- Windows device queries / changes ----------------------------------------------------------------

// True if an accessory-mode phone is attached whose accessory interface needs WinUSB (no driver, a wrong
// class driver, or WinUSB that libusb can't see). `detail` (optional) names the node and what's wrong.
bool AccessoryNeedsDriver(std::string* detail = nullptr);

// Binds Windows' inbox WinUSB driver to every accessory interface that needs it. Requires admin (run by
// the SYSTEM scheduled task or a UAC prompt). Only ever touches nodes ClassifyAccessory() accepts.
// Returns how many devices were bound. The binding persists for that phone.
int BindAccessoryDrivers();

} // namespace mycam
