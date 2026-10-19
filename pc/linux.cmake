# Linux build (docs/LINUX-SUPPORT.md). Included from CMakeLists.txt when not on Windows: mycam-companion (the
# headless daemon), the unit tests, and developer tools. The platform split is per file, never #ifdef in
# shared code: *_posix.cpp, *_openssl.cpp, *_ffmpeg.cpp and *_v4l2.cpp stand in for the Windows files.

find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
# Linked dynamically against the distro's FFmpeg: never vendored or static (MyCam is MIT, FFmpeg LGPL/GPL).
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
# The distro's libusb (the vendored copy in third_party/ has only the Windows backends).
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

# The video path: decoded phone frames into a v4l2loopback camera.
add_library(mycam_video STATIC companion/decoder_ffmpeg.cpp companion/frame_writer_v4l2.cpp vcam/frame_transform.cpp
    companion/log_posix.cpp companion/platform_posix.cpp)
target_link_libraries(mycam_video PUBLIC PkgConfig::FFMPEG)
target_compile_options(mycam_video PRIVATE -Wall -Wextra)

# The page files and status pictures, compiled in (companion.rc's RCDATA on Windows).
set(WEB_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp)
file(GLOB WEB_FILES ${CMAKE_CURRENT_SOURCE_DIR}/companion/web/* ${CMAKE_CURRENT_SOURCE_DIR}/companion/res/frame_*.png)
add_custom_command(OUTPUT ${WEB_ASSETS}
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${WEB_ASSETS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/../design/assets/app-256.png
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Embedding the iPhone page")

# The daemon: the same PhoneLink as the Windows tray app.
add_executable(mycam-companion
    companion/main_linux.cpp
    companion/phone_link.cpp
    companion/phone_link_usb_posix.cpp
    companion/phone_link_wifi.cpp
    companion/phone_link_web.cpp
    companion/web_server.cpp
    companion/web_proto.cpp
    companion/tls_server_openssl.cpp
    companion/wifi_crypto_openssl.cpp
    companion/wifi_crypto_common.cpp
    companion/packet_parser.cpp
    companion/marquee.cpp
    companion/status_images_ffmpeg.cpp
    third_party/qrcodegen/qrcodegen.cpp
    ${WEB_ASSETS})
target_include_directories(mycam-companion PRIVATE companion)
target_link_libraries(mycam-companion PRIVATE mycam_video PkgConfig::LIBUSB OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(mycam-companion PRIVATE -Wall)
install(TARGETS mycam-companion RUNTIME DESTINATION bin)

option(MYCAM_BUILD_TOOLS "Build developer tools (v4l2_play)" ON)
if(MYCAM_BUILD_TOOLS)
    add_executable(v4l2_play tools/v4l2_play.cpp)
    target_link_libraries(v4l2_play PRIVATE mycam_video)
endif()

option(MYCAM_BUILD_TESTS "Build unit tests" ON)
if(MYCAM_BUILD_TESTS)
    enable_testing()
    add_executable(mycam_tests tests/tests.cpp companion/packet_parser.cpp companion/wifi_crypto_openssl.cpp
        companion/wifi_crypto_common.cpp companion/marquee.cpp companion/preview_convert.cpp companion/status_text.cpp
        companion/web_proto.cpp vcam/frame_transform.cpp)
    target_link_libraries(mycam_tests PRIVATE OpenSSL::Crypto)
    target_compile_options(mycam_tests PRIVATE -Wall)
    target_compile_definitions(mycam_tests PRIVATE MYCAM_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt")
    add_test(NAME mycam_tests COMMAND mycam_tests)
    # The OpenSSL TLS server against an OpenSSL client, in memory.
    add_executable(tls_tests tests/tls_test.cpp companion/tls_server_openssl.cpp companion/log_posix.cpp)
    target_link_libraries(tls_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_options(tls_tests PRIVATE -Wall)
    add_test(NAME tls_tests COMMAND tls_tests)
    # The iPhone page's protocol code against the same golden vectors (needs Node.js; skipped without it).
    find_program(NODE_EXE node)
    if(NODE_EXE)
        add_test(NAME web_golden COMMAND ${NODE_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/webtest/golden_check.js
                 ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt)
    endif()
endif()
