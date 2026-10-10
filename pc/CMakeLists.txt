cmake_minimum_required(VERSION 3.20)
project(MyCam LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # Static CRT: no VC++ redist needed.
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/utf-8) # Sources are UTF-8 (status text uses · and ×).

# --- libusb (vendored, static) -------------------------------------------------------------------
set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
add_library(usb STATIC
    ${LIBUSB_DIR}/libusb/core.c
    ${LIBUSB_DIR}/libusb/descriptor.c
    ${LIBUSB_DIR}/libusb/hotplug.c
    ${LIBUSB_DIR}/libusb/io.c
    ${LIBUSB_DIR}/libusb/strerror.c
    ${LIBUSB_DIR}/libusb/sync.c
    ${LIBUSB_DIR}/libusb/os/events_windows.c
    ${LIBUSB_DIR}/libusb/os/threads_windows.c
    ${LIBUSB_DIR}/libusb/os/windows_common.c
    ${LIBUSB_DIR}/libusb/os/windows_usbdk.c
    ${LIBUSB_DIR}/libusb/os/windows_winusb.c
)
target_include_directories(usb PUBLIC ${LIBUSB_DIR}/libusb PRIVATE ${LIBUSB_DIR}/msvc)
target_compile_options(usb PRIVATE /W0)

# --- Virtual camera media source (loaded by Windows Frame Server) --------------------------------
add_library(MyCamVCam SHARED
    vcam/dllmain.cpp
    vcam/media_source.cpp
    vcam/frame_reader.cpp
    vcam/frame_transform.cpp
    vcam/MyCamVCam.def
)
target_link_libraries(MyCamVCam PRIVATE mfplat mfuuid advapi32 runtimeobject)

# --- Companion tray app ---------------------------------------------------------------------------
add_executable(MyCamCompanion WIN32
    companion/main.cpp
    companion/phone_link.cpp
    companion/decoder.cpp
    companion/frame_writer.cpp
    companion/packet_parser.cpp
    companion/winusb_bind.cpp
    companion/driver_task.cpp
    companion/log.cpp
    companion/app_settings.cpp
    companion/status_text.cpp
    companion/status_images.cpp
    companion/marquee.cpp
    vcam/frame_transform.cpp
    companion/settings_window.cpp
    companion/companion.rc
    companion/companion.manifest
)
target_link_libraries(MyCamCompanion PRIVATE usb mfplat mfuuid mfsensorgroup ole32 shell32 advapi32 setupapi newdev cfgmgr32
    comctl32 d2d1 dwrite windowscodecs dwmapi wtsapi32 taskschd)

# --- Developer tools (not shipped; install.ps1 copies only the two binaries above) -------------------
option(MYCAM_BUILD_TOOLS "Build developer tools (capture_test, usb_probe)" OFF)
if(MYCAM_BUILD_TOOLS)
    add_executable(capture_test tools/capture_test.cpp)
    target_link_libraries(capture_test PRIVATE mfplat mf mfreadwrite mfuuid ole32)
    add_executable(usb_probe tools/usb_probe.cpp)
    target_link_libraries(usb_probe PRIVATE usb)
endif()

# --- Unit tests ------------------------------------------------------------------------------------
option(MYCAM_BUILD_TESTS "Build unit tests" ON)
if(MYCAM_BUILD_TESTS)
    enable_testing()
    add_executable(mycam_tests tests/tests.cpp companion/packet_parser.cpp companion/marquee.cpp vcam/frame_transform.cpp)
    target_compile_definitions(mycam_tests PRIVATE MYCAM_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt")
    add_test(NAME mycam_tests COMMAND mycam_tests)
endif()
