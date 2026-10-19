#pragma once
// Which formats the virtual cameras offer apps (#10). The base formats are always offered, so a camera
// opened before any phone has connected still works; 4K, 60 and 120 fps only once a phone has reported
// them. The companion stores that as a bitmask under HKLM, which both cameras can read: MyCamVCam.dll
// inside the Frame Server service and MyCamVCamDShow.dll inside whatever app opened the camera.
// Apps read the list when they open the camera, so one already open keeps its list until it reopens.

#include <stddef.h>
#include <stdint.h>

namespace mycam {

enum VcamModeBits : uint32_t {
    kMode4K30 = 0x01, kMode1080p60 = 0x02, kMode720p60 = 0x04, kMode1080p120 = 0x08, kMode720p120 = 0x10,
};
constexpr uint32_t kAllVcamModes = 0x1F;

// `needs`: the kMode* bit the format depends on (0: always offered). The first is the default.
struct VcamFormat { uint32_t width, height, fps, needs; };
constexpr VcamFormat kVcamFormats[] = {
    {1920, 1080, 30, 0}, {1280, 720, 30, 0}, {3840, 2160, 30, kMode4K30}, {1920, 1080, 60, kMode1080p60},
    {1280, 720, 60, kMode720p60}, {1920, 1080, 120, kMode1080p120}, {1280, 720, 120, kMode720p120},
    {640, 480, 30, 0}, {640, 360, 30, 0},
};
constexpr size_t kVcamFormatCount = sizeof(kVcamFormats) / sizeof(kVcamFormats[0]);

// Copies the formats `modes` allows into `out` (room for kVcamFormatCount), in list order; returns how many.
inline size_t OfferedFormats(uint32_t modes, VcamFormat* out) {
    size_t n = 0;
    for (const VcamFormat& f : kVcamFormats) {
        if ((f.needs & modes) == f.needs) out[n++] = f;
    }
    return n;
}

} // namespace mycam

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>

namespace mycam {

// HKLM\SOFTWARE\MyCam, value VCamModes (DWORD of kMode* bits). DllRegisterServer creates the key and
// lets interactive users write it, since the companion runs unelevated.
constexpr wchar_t kVcamModesKey[] = L"SOFTWARE\\MyCam";
constexpr wchar_t kVcamModesValue[] = L"VCamModes";
constexpr wchar_t kVcamModesSddl[] = L"D:(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KRKW;;;IU)(A;CI;KR;;;BU)(A;CI;KR;;;LS)";

// No value yet (no phone has connected since install): base formats only.
inline uint32_t ReadVcamModes() {
    DWORD modes = 0, size = sizeof(modes);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kVcamModesKey, kVcamModesValue, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                     nullptr, &modes, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return modes & kAllVcamModes;
}

inline LSTATUS WriteVcamModes(uint32_t modes) {
    HKEY key;
    LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVcamModesKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (st != ERROR_SUCCESS) return st;
    const DWORD value = modes;
    st = RegSetValueExW(key, kVcamModesValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return st;
}

// For DllRegisterServer (elevated): creates the key and (re)applies kVcamModesSddl, also to an existing key.
inline HRESULT CreateVcamModesKey() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kVcamModesSddl, SDDL_REVISION_1, &sd, nullptr)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HKEY key;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kVcamModesKey, 0, nullptr, 0, WRITE_DAC | KEY_WOW64_64KEY,
                                 nullptr, &key, nullptr);
    if (st == ERROR_SUCCESS) {
        st = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, sd);
        RegCloseKey(key);
    }
    LocalFree(sd);
    return HRESULT_FROM_WIN32(st);
}

inline void DeleteVcamModesKey() {
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kVcamModesKey, KEY_WOW64_64KEY, 0);
}

} // namespace mycam
#endif
