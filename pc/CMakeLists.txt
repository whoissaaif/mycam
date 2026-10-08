cmake_minimum_required(VERSION 3.20)
project(MyCam LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # Static CRT: no VC++ redist needed.
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

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
    vcam/MyCamVCam.def
)
target_link_libraries(MyCamVCam PRIVATE mfplat mfuuid advapi32 runtimeobject)

# --- Companion tray app ---------------------------------------------------------------------------
add_executable(MyCamCompanion WIN32
    companion/main.cpp
    companion/phone_link.cpp
    companion/decoder.cpp
    companion/frame_writer.cpp
    companion/winusb_bind.cpp
    companion/log.cpp
    companion/companion.rc
)
target_link_libraries(MyCamCompanion PRIVATE usb mfplat mfuuid mfsensorgroup ole32 shell32 advapi32 setupapi newdev cfgmgr32)

# --- Test tool: grabs a frame from the MyCam camera ------------------------------------------------
add_executable(capture_test tools/capture_test.cpp)
target_link_libraries(capture_test PRIVATE mfplat mf mfreadwrite mfuuid ole32)
add_executable(usb_probe tools/usb_probe.cpp)
target_link_libraries(usb_probe PRIVATE usb)
