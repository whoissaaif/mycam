#pragma once
// The MyCam main window, drawn in the Windows XP "Luna" style (redesign.md §8): an XP task pane on the
// left (status and Pause, then collapsible Camera / Video / Picture / Wi-Fi groups and task links), the
// live preview with the camera controls on the right. Direct2D device context + DirectComposition
// (xp_draw.h), a small layout engine (ui_layout.h) and an element model (ui_model.h) that keyboard
// navigation uses and a UI Automation provider can build on.

#include <windows.h>

#include <functional>
#include <vector>

#include "phone_link.h"
#include "ui_model.h"

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
    std::function<bool()> paused;
    std::function<void(bool)> setPaused;
    std::function<bool()> fill;
    std::function<void(bool)> setFill;
    std::function<void(uint8_t, uint8_t)> command; // v3 camera command (proto::Command, arg)
    std::function<bool()> wireless;                // "Find phones on Wi-Fi" (beta)
    std::function<void(bool)> setWireless;
    std::function<void()> forgetPhones;            // Forget Wi-Fi phones
    std::function<void()> showPairing;             // Opens the pairing dialog (headline click while pairing)
    bool testPattern = false;                      // --test-pattern: the preview shows the pattern as live
};

class SettingsWindow {
public:
    explicit SettingsWindow(SettingsModel model) : model_(std::move(model)) {}
    ~SettingsWindow();

    void Show();               // Creates the window, or brings it to the front.
    void Refresh();            // Status or settings changed elsewhere; repaint if open.
    HWND Hwnd() const;         // nullptr while closed.

    // --- Element model (for keyboard navigation and a UI Automation provider) ----------------------
    // Every element of the current layout, in visual (= tab) order. Rects are client DIPs; use
    // ClientDipsToScreen() for UIA's BoundingRectangle. Empty while the window is closed.
    std::vector<ui::Element> Elements() const;
    bool Invoke(int id);       // What a click / Space does (toggle, select, expand/collapse, press).
    bool Focus(int id);        // Moves keyboard focus (and scrolls the task pane to show it).
    int FocusedId() const;
    RECT ClientDipsToScreen(const ui::Box& box) const;
    // Called (on the UI thread) after a layout whose elements or states differ from the previous one.
    void SetElementsChanged(std::function<void()> callback);

private:
    struct Impl;
    SettingsModel model_;
    Impl* impl_ = nullptr;
    std::function<void()> elementsChanged_;
};

} // namespace mycam
