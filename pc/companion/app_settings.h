#pragma once
// Per-user settings (HKCU\Software\MyCam) and the "Start with Windows" Run entry.

#include <windows.h>

namespace mycam {

DWORD ReadSetting(const wchar_t* name, DWORD fallback);
void WriteSetting(const wchar_t* name, DWORD value);

bool AutostartEnabled();
void SetAutostart(bool enable);

// Opens %LOCALAPPDATA%\MyCam (where mycam.log lives) in Explorer.
void OpenLogFolder();

// Where the About page's contact link writes to.
constexpr wchar_t kSupportEmail[] = L"support@mycam.dev";

// Starts an email to kSupportEmail in the default mail app.
void OpenSupportEmail();

} // namespace mycam
