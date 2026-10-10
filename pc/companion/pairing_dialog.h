#pragma once
// The XP-styled pairing dialog (redesign.md §8.3, W4): the phone's name, the 6-digit code in Trebuchet MS
// Bold 24 pt (never truncated; the name gets the ellipsis), a green determinate bar counting down the
// time the phone gives its user, and "Cancel pairing", which makes the PC close the socket (no protocol
// change). It opens when a pairing code appears and closes when the pairing resolves.

#include <windows.h>

#include <functional>
#include <vector>

#include "phone_link.h"
#include "ui_model.h"

namespace mycam {

struct PairingModel {
    std::function<LinkStatus()> status;
    std::function<void()> cancel; // PhoneLink::CancelPairing
};

class PairingDialog {
public:
    explicit PairingDialog(PairingModel model) : model_(std::move(model)) {}
    ~PairingDialog();

    // Opens the dialog for the current pairing code (or brings it to the front). `owner` (optional) is
    // disabled while the dialog is open, as with any modal dialog. No-op when nothing is pairing.
    void Show(HWND owner);
    void Close();
    // Status changed: follows a new code, closes once the code is gone.
    void Refresh();
    bool IsOpen() const;
    HWND Hwnd() const;
    // Same element model as the settings window (for keyboard navigation and UI Automation).
    std::vector<ui::Element> Elements() const;

private:
    struct Impl;
    PairingModel model_;
    Impl* impl_ = nullptr;
};

} // namespace mycam
