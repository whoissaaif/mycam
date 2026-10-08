#pragma once
// One description of the link state, shared by the tray icon/tooltip and the settings window.

#include <string>

#include "phone_link.h"

namespace mycam {

// Resource IDs of the tray icons (companion.rc).
enum TrayIconId : int {
    kIconApp = 1,
    kIconDisconnected = 101,
    kIconReady = 102,
    kIconStreaming = 103,
    kIconPaused = 104,
    kIconError = 105,
};

struct StatusView {
    std::wstring headline; // "Streaming"
    std::wstring detail;   // "Back camera · 1920 × 1080"
    TrayIconId icon;
};

StatusView DescribeStatus(const LinkStatus& status, bool cameraRegistered);

} // namespace mycam
