#pragma once
#include <guiddef.h>

namespace mycam {

// COM class of the virtual camera media source (MyCamVCam.dll). Registered by install.ps1 via regsvr32.
#define MYCAM_VCAM_CLSID_STRING L"{7B3C1E2A-9F4D-4C8B-A6E5-2D1F0B9C8A71}"
constexpr GUID CLSID_MyCamVCam = {0x7b3c1e2a, 0x9f4d, 0x4c8b, {0xa6, 0xe5, 0x2d, 0x1f, 0x0b, 0x9c, 0x8a, 0x71}};

constexpr wchar_t kCameraFriendlyName[] = L"MyCam";

} // namespace mycam
