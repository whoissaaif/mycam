#include "status_text.h"

#include "protocol.h"

namespace mycam {

StatusView DescribeStatus(const LinkStatus& s, bool cameraRegistered) {
    if (!cameraRegistered) {
        return {L"Camera not registered", L"Run the MyCam installer again. Windows 11 is required.", kIconError};
    }
    const std::wstring camera = s.facing == proto::kFacingFront ? L"Front camera" : L"Back camera";
    if (s.lockPaused && s.state != LinkState::NoDriver) {
        return {L"Paused", L"Windows is locked. MyCam turns the camera back on when you unlock.", kIconPaused};
    }
    switch (s.state) {
    case LinkState::NoDriver:
        return {L"USB driver missing", L"Run the MyCam installer again to set up the USB driver.", kIconError};
    case LinkState::Searching:
        return {L"No phone connected", L"Plug in your phone with a USB cable.", kIconDisconnected};
    case LinkState::Waiting:
        return {L"Phone found", L"Open MyCam on the phone, and tap OK if it asks.", kIconDisconnected};
    case LinkState::Idle:
        return {L"Ready", camera + L". Choose “MyCam” as the camera in any app.", kIconReady};
    case LinkState::Streaming: {
        std::wstring detail = camera;
        if (s.width) detail += L" · " + std::to_wstring(s.width) + L" × " + std::to_wstring(s.height);
        return {L"Streaming", detail, kIconStreaming};
    }
    case LinkState::PhoneError:
        return {L"Camera problem", L"Check the phone. Another app may be using its camera.", kIconError};
    case LinkState::Paused:
        return {L"Paused", L"The camera is off. Apps see a “Camera paused” picture. Resume here or on the phone.", kIconPaused};
    }
    return {L"", L"", kIconDisconnected};
}

} // namespace mycam
