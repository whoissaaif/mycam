#pragma once

namespace mycam {

// True if a phone in accessory mode is attached with no driver (Windows has none for 18d1:2d0x).
bool AccessoryNeedsDriver();

// Binds Windows' inbox WinUSB driver to every driverless accessory-mode phone. Requires admin.
// Returns how many devices were bound. The binding persists for that phone.
int BindAccessoryDrivers();

} // namespace mycam
