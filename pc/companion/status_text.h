#pragma once
// One description of the link state, shared by the tray icon/tooltip, the settings window and the pairing
// dialog (redesign.md §2.3: one icon, one main instruction, one grey sentence).

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

// Progress the window shows under the status (redesign.md §6.1): never in steady states.
enum class StatusProgress { None, Working, Pairing };

struct StatusView {
    std::wstring headline; // "Streaming"
    std::wstring detail;   // "Back camera · 1920 × 1080" (tray tooltip and balloons)
    TrayIconId icon;
    std::wstring facts;    // Short line under the headline in the window: "Back camera · 1920 × 1080 · USB"
    std::wstring hint;     // The sentence under the preview: "Choose “MyCam” as the camera in any app."
    StatusProgress progress = StatusProgress::None;
};

StatusView DescribeStatus(const LinkStatus& status, bool cameraRegistered);

} // namespace mycam
