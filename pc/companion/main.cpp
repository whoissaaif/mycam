// MyCamCompanion.exe: tray app that registers the "MyCam" virtual camera and bridges the phone over USB.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/guids.h"
#include "app_settings.h"
#include "log.h"
#include "phone_link.h"
#include "protocol.h"
#include "settings_window.h"
#include "status_images.h"
#include "status_text.h"
#include "winusb_bind.h"

using Microsoft::WRL::ComPtr;
using namespace mycam;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_STATUS = WM_APP + 2;
constexpr UINT WM_NEED_DRIVER = WM_APP + 3;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kMenuSettings = 100,
    kMenuStatus,
    kMenuPause,
    kMenuBack,
    kMenuFront,
    kMenuMirror,
    kMenuReconnect,
    kMenuAutostart,
    kMenuExit,
};

HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_tray = {};
PhoneLink* g_link = nullptr;
SettingsWindow* g_settings = nullptr;
bool g_mirror = false;

std::mutex g_statusLock;
LinkStatus g_status;
LinkStatus g_shownStatus;
bool g_vcamOk = false;
bool g_sessionLocked = false; // Windows session locked (WTS notifications)
bool g_suspended = false;     // PC going to sleep
int g_trayIcon = 0;

LinkStatus CurrentStatus() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    return g_status;
}

// Tray icons are drawn for 16-32 px; LoadIconMetric picks the right size for the current DPI.
HICON LoadTrayIcon(int id) {
    HICON icon = nullptr;
    if (FAILED(LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), LIM_SMALL, &icon))) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

void Notify(const wchar_t* title, const std::wstring& text) {
    g_tray.uFlags = NIF_INFO;
    wcsncpy_s(g_tray.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_tray.szInfo, text.c_str(), _TRUNCATE);
    g_tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void UpdateTray() {
    const LinkStatus s = CurrentStatus();
    const StatusView view = DescribeStatus(s, g_vcamOk);

    g_tray.uFlags = NIF_TIP;
    std::wstring tip = L"MyCam: " + view.headline + L"\n" + view.detail;
    wcsncpy_s(g_tray.szTip, tip.c_str(), _TRUNCATE);
    if (view.icon != g_trayIcon) {
        if (g_tray.hIcon) DestroyIcon(g_tray.hIcon);
        g_tray.hIcon = LoadTrayIcon(view.icon);
        g_tray.uFlags |= NIF_ICON;
        g_trayIcon = view.icon;
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    if (g_settings) g_settings->Refresh();

    // Balloon only on meaningful transitions.
    bool wasConnected = g_shownStatus.state >= LinkState::Idle;
    bool isConnected = s.state >= LinkState::Idle;
    if (s.state == LinkState::NoDriver && g_shownStatus.state != LinkState::NoDriver) {
        Notify(view.headline.c_str(), view.detail);
    } else if (isConnected && !wasConnected) {
        Notify(L"Phone connected", L"Choose “MyCam” as the camera in any app.");
    } else if (s.state == LinkState::Waiting && g_shownStatus.state != LinkState::Waiting) {
        Notify(L"Phone found", L"Tap OK on your phone to open MyCam (tick “Always” so you're never asked again).");
    }
    g_shownStatus = s;
}

void SetMirror(bool mirror) {
    g_mirror = mirror;
    g_link->SetMirror(mirror);
    WriteSetting(L"Mirror", mirror);
}

// Windows locked or asleep: keep the phone camera off (privacy). Separate from the user's own pause.
void UpdateLockPause() {
    if (g_link) g_link->SetLockPaused(g_sessionLocked || g_suspended);
}

void ShowMenu(HWND hwnd) {
    const LinkStatus s = CurrentStatus();
    const StatusView view = DescribeStatus(s, g_vcamOk);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"&Settings…");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE); // Bold; also what a left-click opens.
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, view.headline.c_str());
    const bool paused = s.state == LinkState::Paused;
    const bool connected = s.state >= LinkState::Idle;
    AppendMenuW(menu, MF_STRING | (connected && !s.lockPaused ? 0 : MF_GRAYED), kMenuPause,
                paused ? L"Resume camera" : L"Pause camera");
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
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP: if (g_settings) g_settings->Show(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: ShowMenu(hwnd); break;
        }
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_UNLOCK) {
            g_sessionLocked = wp == WTS_SESSION_LOCK;
            UpdateLockPause();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) g_suspended = true;
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) g_suspended = false;
        UpdateLockPause();
        return TRUE;
    case WM_NEED_DRIVER:
        RunDriverBinder();
        return 0;
    case WM_STATUS:
        UpdateTray();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuSettings: if (g_settings) g_settings->Show(); break;
        case kMenuPause: g_link->RequestPause(CurrentStatus().state != LinkState::Paused); break;
        case kMenuBack:  g_link->RequestFacing(proto::kFacingBack); break;
        case kMenuFront: g_link->RequestFacing(proto::kFacingFront); break;
        case kMenuMirror: SetMirror(!g_mirror); break;
        case kMenuReconnect: g_link->RequestReconnect(); break;
        case kMenuAutostart: SetAutostart(!AutostartEnabled()); break;
        case kMenuExit: DestroyWindow(hwnd); break;
        }
        if (g_settings) g_settings->Refresh();
        return 0;
    case WM_DESTROY:
        WTSUnRegisterSessionNotification(hwnd);
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

int QuitRunningInstance() {
    // Current versions use a hidden top-level window; versions before 1.1 used a message-only window.
    HWND other = FindWindowW(L"MyCamCompanion", nullptr);
    if (!other) other = FindWindowExW(HWND_MESSAGE, nullptr, L"MyCamCompanion", nullptr);
    if (!other) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(other, WM_CLOSE, 0, 0);
    DWORD wait = process ? WaitForSingleObject(process, 5000) : WAIT_FAILED;
    if (process) CloseHandle(process);
    return wait == WAIT_OBJECT_0 ? 0 : 1;
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
    // Used by the installer/uninstaller: close the running companion cleanly (it turns the phone camera
    // off on the way out) and wait for it to exit. Exit code 0 = none running or it closed.
    if (wcsstr(GetCommandLineW(), L"--quit")) return QuitRunningInstance();

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
    // A hidden top-level window (never shown): unlike a message-only window it receives lock/unlock and
    // sleep notifications.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"MyCam", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);

    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_hwnd;
    g_tray.uID = kTrayId;
    g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadTrayIcon(kIconDisconnected);
    g_trayIcon = kIconDisconnected;
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
    link.SetStatusImages(LoadStatusImage(kImagePaused), LoadStatusImage(kImageWaiting));
    g_link = &link;

    SettingsWindow settings({
        CurrentStatus,
        [] { return g_vcamOk; },
        [] { return g_mirror; },
        SetMirror,
        AutostartEnabled,
        SetAutostart,
        [](int facing) { g_link->RequestFacing(facing); },
        [] { g_link->RequestReconnect(); },
        OpenLogFolder,
        [] { return CurrentStatus().state == LinkState::Paused; },
        [](bool pause) { g_link->RequestPause(pause); },
    });
    g_settings = &settings;
    UpdateTray();
    if (wcsstr(GetCommandLineW(), L"--settings")) settings.Show();

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
    g_settings = nullptr;
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
