#pragma once
// MyCam settings window, drawn in the Windows 7 "Aero" style (IMPROVEMENTS.md section 7) with Direct2D.

#include <windows.h>

#include <functional>

#include "phone_link.h"

namespace mycam {

struct SettingsModel {
    std::function<LinkStatus()> status;
    std::function<bool()> cameraRegistered;
    std::function<bool()> mirror;
    std::function<void(bool)> setMirror;
    std::function<bool()> autostart;
    std::function<void(bool)> setAutostart;
    std::function<void(int)> setFacing; // proto::Facing
    std::function<void()> reconnect;
    std::function<void()> openLogFolder;
};

class SettingsWindow {
public:
    explicit SettingsWindow(SettingsModel model) : model_(std::move(model)) {}
    ~SettingsWindow();

    void Show();               // Creates the window, or brings it to the front.
    void Refresh();            // Status or settings changed elsewhere; repaint if open.

private:
    struct Impl;
    SettingsModel model_;
    Impl* impl_ = nullptr;
};

} // namespace mycam
