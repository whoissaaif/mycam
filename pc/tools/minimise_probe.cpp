// minimise_probe: measures what minimising a window does to this process's H.264 decoding (#95).
//
// It drives the companion's real H264Decoder on a worker thread from an Annex-B file, timed exactly as
// phone_link.cpp times it, while the main thread owns a window it minimises and restores on its own. One
// line a second lines the decode average up with the window state, so no phone (and no clicking) is needed:
//
//   minimise_probe sample.h264            -> ~7 ms visible, ~62 ms minimised or hidden (Windows 11)
//   minimise_probe sample.h264 --fix      -> ~7 ms throughout: what main.cpp's KeepTimerResolution does
//
// Any Annex-B H.264 file will do (the phone's stream, or ffmpeg -i any.mp4 -c:v libx264 -bsf:v h264_mp4toannexb
// -f h264 sample.h264). Build with -DMYCAM_BUILD_TOOLS=ON.
#include <windows.h>
#include <stdio.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../companion/decoder.h"

namespace {

// The file's access units: NALs grouped so each buffer ends with a slice, which is what PhoneLink hands
// Decode() one frame at a time.
std::vector<std::vector<uint8_t>> LoadAccessUnits(const char* path) {
    std::vector<std::vector<uint8_t>> units;
    FILE* f = fopen(path, "rb");
    if (!f) return units;
    std::vector<uint8_t> all;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.insert(all.end(), buf, buf + n);
    fclose(f);

    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < all.size(); ++i) {
        if (all[i] == 0 && all[i + 1] == 0 && all[i + 2] == 1) starts.push_back(i);
    }
    std::vector<uint8_t> unit;
    for (size_t s = 0; s < starts.size(); ++s) {
        const size_t begin = starts[s];
        const size_t end = s + 1 < starts.size() ? starts[s + 1] : all.size();
        const uint8_t type = all[begin + 3] & 0x1f;
        unit.insert(unit.end(), all.begin() + begin, all.begin() + end);
        if (type == 1 || type == 5) { // A slice ends the access unit.
            units.push_back(unit);
            unit.clear();
        }
    }
    return units;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: minimise_probe <file.h264> [--fix]\n");
        return 2;
    }
    if (argc > 2 && std::string(argv[2]) == "--fix") {
        PROCESS_POWER_THROTTLING_STATE state = {};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0;
        printf("timer-resolution opt-out: %d\n",
               int(SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))));
    }

    const auto units = LoadAccessUnits(argv[1]);
    if (units.size() < 2) {
        printf("no access units in %s\n", argv[1]);
        return 1;
    }
    printf("%zu access units\n", units.size());

    MFStartup(MF_VERSION);
    std::atomic<bool> quit{false};
    std::atomic<int> frames{0};
    std::atomic<double> decodeMs{0};

    std::thread worker([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
        mycam::H264Decoder decoder;
        const HRESULT hr = decoder.Init(1920, 1080, [](const mycam::H264Decoder::Nv12View&) {});
        printf("decoder init -> 0x%08X, on the GPU: %d\n", unsigned(hr), int(decoder.OnGpu()));
        fflush(stdout);
        int64_t pts = 0;
        for (size_t i = 0; !quit; ++i) {
            const auto& unit = units[i % units.size()];
            const auto t0 = std::chrono::steady_clock::now();
            decoder.Decode(unit.data(), unit.size(), pts);
            decodeMs = decodeMs + std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0).count();
            ++frames;
            pts += 16666;
            Sleep(1); // Never a tight spin: the real loop waits on the link between frames.
        }
        MFShutdown();
        CoUninitialize();
    });

    WNDCLASSW cls = {};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"MyCamMinimiseProbe";
    RegisterClassW(&cls);
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"MyCam minimise probe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 720, 500, nullptr, nullptr, cls.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    const auto start = std::chrono::steady_clock::now();
    int lastSecond = -1, lastFrames = 0;
    double lastMs = 0;
    const char* phase = "visible";
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const int secs = int(std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - start).count());
        if (secs == 8 && phase[0] == 'v') {
            ShowWindow(hwnd, SW_MINIMIZE);
            phase = "MINIMISED";
        }
        if (secs == 18 && phase[0] == 'M') {
            ShowWindow(hwnd, SW_HIDE); // No window at all: MyCam closed to the tray, its normal state.
            phase = "HIDDEN";
        }
        if (secs == 28 && phase[0] == 'H') {
            ShowWindow(hwnd, SW_RESTORE); // SW_SHOW alone would put back the minimised state it was hidden in.
            phase = "restored";
        }
        if (secs != lastSecond) {
            lastSecond = secs;
            const int f = frames;
            const double ms = decodeMs;
            const int df = f - lastFrames;
            printf("%2ds %-10s %3d frames/s, decode avg %6.2f ms\n", secs, phase, df,
                   df ? (ms - lastMs) / df : 0.0);
            fflush(stdout);
            lastFrames = f;
            lastMs = ms;
        }
        if (secs >= 36) break;
        Sleep(20);
    }
    quit = true;
    worker.join();
    MFShutdown();
    return 0;
}
