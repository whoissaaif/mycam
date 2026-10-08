// MyCamCompanion.exe: tray app that registers the "MyCam" virtual camera and bridges the phone over USB.

#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/guids.h"
#include "phone_link.h"
#include "log.h"
#include "winusb_bind.h"
#include "protocol.h"

using Microsoft::WRL::ComPtr;
using namespace mycam;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_STATUS = WM_APP + 2;
constexpr UINT WM_NEED_DRIVER = WM_APP + 3;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kMenuStatus = 100,
    kMenuBack,
    kMenuFront,
    kMenuMirror,
    kMenuReconnect,
    kMenuAutostart,
    kMenuExit,
};

constexpr wchar_t kSettingsKey[] = L"Software\\MyCam";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MyCam";

HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_tray = {};
PhoneLink* g_link = nullptr;
bool g_mirror = false;

std::mutex g_statusLock;
LinkStatus g_status;
LinkStatus g_shownStatus;
bool g_vcamOk = false;

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}

void WriteSetting(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof(value));
}

bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, kRunValue);
            RegCloseKey(key);
        }
    }
}

std::wstring StatusText(const LinkStatus& s) {
    if (!g_vcamOk) return L"Virtual camera unavailable (run install.ps1)";
    const wchar_t* cam = s.facing == proto::kFacingFront ? L"front" : L"back";
    switch (s.state) {
    case LinkState::NoDriver:   return L"USB driver missing (run install.ps1)";
    case LinkState::Searching:  return L"Plug in your phone";
    case LinkState::Waiting:    return L"Phone found: open MyCam on the phone";
    case LinkState::Idle:       return std::wstring(L"Ready (") + cam + L" camera)";
    case LinkState::PhoneError: return L"Phone camera problem; check the phone";
    case LinkState::Streaming:
        if (s.width) return std::wstring(L"Streaming ") + cam + L" camera, " + std::to_wstring(s.width) + L"x" + std::to_wstring(s.height);
        return std::wstring(L"Streaming ") + cam + L" camera";
    }
    return L"";
}

void Notify(const wchar_t* title, const std::wstring& text) {
    g_tray.uFlags = NIF_INFO;
    wcsncpy_s(g_tray.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_tray.szInfo, text.c_str(), _TRUNCATE);
    g_tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void UpdateTray() {
    LinkStatus s;
    {
        std::lock_guard<std::mutex> lock(g_statusLock);
        s = g_status;
    }
    std::wstring tip = L"MyCam: " + StatusText(s);
    g_tray.uFlags = NIF_TIP;
    wcsncpy_s(g_tray.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);

    // Balloon only on meaningful transitions.
    bool wasConnected = g_shownStatus.state >= LinkState::Idle;
    bool isConnected = s.state >= LinkState::Idle;
    if (s.state == LinkState::NoDriver && g_shownStatus.state != LinkState::NoDriver) {
        Notify(L"MyCam", StatusText(s));
    } else if (isConnected && !wasConnected) {
        Notify(L"Phone connected", L"Select \"MyCam\" as the camera in any app.");
    } else if (s.state == LinkState::Waiting && g_shownStatus.state != LinkState::Waiting) {
        Notify(L"Phone found", L"Tap OK on your phone to open MyCam (tick \"Always\" so you're never asked again).");
    }
    g_shownStatus = s;
}

void ShowMenu(HWND hwnd) {
    LinkStatus s;
    {
        std::lock_guard<std::mutex> lock(g_statusLock);
        s = g_status;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, StatusText(s).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuBack, L"Back camera");
    AppendMenuW(menu, MF_STRING, kMenuFront, L"Front camera");
    CheckMenuRadioItem(menu, kMenuBack, kMenuFront, s.facing == proto::kFacingFront ? kMenuFront : kMenuBack, MF_BYCOMMAND);
    AppendMenuW(menu, MF_STRING | (g_mirror ? MF_CHECKED : 0), kMenuMirror, L"Mirror image");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuReconnect, L"Reconnect phone");
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Runs "MyCamCompanion.exe --bind-driver" elevated to give an accessory-mode phone the WinUSB driver.
// Windows asks for admin once per phone; afterwards the binding persists.
void RunDriverBinder() {
    static HANDLE running = nullptr;
    if (running) {
        if (WaitForSingleObject(running, 0) == WAIT_TIMEOUT) return;
        CloseHandle(running);
        running = nullptr;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = L"--bind-driver";
    sei.nShow = SW_HIDE;
    Notify(L"Setting up your phone", L"Windows will ask for permission once to install the USB driver for this phone.");
    if (ShellExecuteExW(&sei)) running = sei.hProcess;
    else Notify(L"MyCam", L"Permission was declined. Unplug and replug the phone to try again.");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu(hwnd);
        return 0;
    case WM_NEED_DRIVER:
        RunDriverBinder();
        return 0;
    case WM_STATUS:
        UpdateTray();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuBack:  g_link->RequestFacing(proto::kFacingBack); break;
        case kMenuFront: g_link->RequestFacing(proto::kFacingFront); break;
        case kMenuMirror:
            g_mirror = !g_mirror;
            g_link->SetMirror(g_mirror);
            WriteSetting(L"Mirror", g_mirror);
            break;
        case kMenuReconnect: g_link->RequestReconnect(); break;
        case kMenuAutostart: SetAutostart(!AutostartEnabled()); break;
        case kMenuExit: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        PostQuitMessage(0);
        return 0;
    }
    // Explorer restarted: re-add the tray icon.
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &g_tray);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --test-pattern: feeds moving colour bars instead of the phone, to check the virtual camera alone.
void RunTestPattern(std::atomic<bool>& quit, uint32_t rotation) {
    constexpr uint32_t w = 1280, h = 720;
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                       {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    std::vector<uint8_t> frame(w * h * 3 / 2);
    FrameWriter writer;
    for (uint32_t n = 0; !quit; ++n) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) frame[y * w + x] = bars[(x * 8 / w + n / 30) % 8][0];
        for (uint32_t y = 0; y < h / 2; ++y)
            for (uint32_t x = 0; x < w / 2; ++x) {
                const uint8_t* c = bars[(x * 16 / w + n / 30) % 8];
                frame[w * h + y * w + x * 2] = c[1];
                frame[w * h + y * w + x * 2 + 1] = c[2];
            }
        uint32_t line = n * 8 % h; // White line sweeping down shows the feed is live.
        memset(&frame[line * w], 235, w);
        writer.Write(frame.data(), w, h, rotation, false);
        Sleep(33);
    }
}

HRESULT CreateVirtualCamera(ComPtr<IMFVirtualCamera>& vcam) {
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                                       MFVirtualCameraAccess_CurrentUser, kCameraFriendlyName, MYCAM_VCAM_CLSID_STRING,
                                       nullptr, 0, &vcam);
    if (SUCCEEDED(hr)) hr = vcam->Start(nullptr);
    return hr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Elevated helper mode (launched by the companion via UAC): bind WinUSB to accessory-mode phones.
    if (wcsstr(GetCommandLineW(), L"--bind-driver")) return BindAccessoryDrivers();

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MyCamCompanion");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    LogInit();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFVirtualCamera> vcam;
    HRESULT vcamHr = CreateVirtualCamera(vcam);
    g_vcamOk = SUCCEEDED(vcamHr);
    Log("companion started; virtual camera -> 0x%08X", unsigned(vcamHr));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"MyCamCompanion";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"MyCam", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);

    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_hwnd;
    g_tray.uID = kTrayId;
    g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"MyCam");
    Shell_NotifyIconW(NIM_ADD, &g_tray);

    if (!g_vcamOk) {
        wchar_t msg[256];
        swprintf_s(msg, L"Could not register the MyCam virtual camera (0x%08X). Windows 11 is required, and "
                        L"install.ps1 must have been run once as administrator.", unsigned(vcamHr));
        Notify(L"MyCam", msg);
    }

    g_mirror = ReadSetting(L"Mirror", 0) != 0;
    PhoneLink link([](const LinkStatus& s) {
        {
            std::lock_guard<std::mutex> lock(g_statusLock);
            g_status = s;
        }
        PostMessageW(g_hwnd, WM_STATUS, 0, 0);
    }, [] { PostMessageW(g_hwnd, WM_NEED_DRIVER, 0, 0); });
    link.SetMirror(g_mirror);
    g_link = &link;
    std::atomic<bool> quitPattern{false};
    const bool testPattern = wcsstr(GetCommandLineW(), L"--test-pattern") != nullptr;
    std::thread worker([&] {
        if (testPattern) {
            const wchar_t* rot = wcsstr(GetCommandLineW(), L"--rotate=");
            RunTestPattern(quitPattern, rot ? uint32_t(_wtoi(rot + 9)) : 0);
            return;
        }
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
        link.Run();
        MFShutdown();
        CoUninitialize();
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    link.Quit();
    quitPattern = true;
    worker.join();
    g_link = nullptr;
    if (vcam) {
        vcam->Stop();
        vcam->Shutdown();
    }
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
