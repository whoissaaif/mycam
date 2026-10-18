# Linux build (docs/LINUX-SUPPORT.md). Included from CMakeLists.txt when not on Windows. So far: the unit
# tests (the portable companion code, with OpenSSL standing in for Windows CNG) and the video path (FFmpeg
# decoder + v4l2loopback writer), with a developer tool to drive it.

find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
# Linked dynamically against the distro's FFmpeg: never vendored or static (MyCam is MIT, FFmpeg LGPL/GPL).
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

# The video path: decoded phone frames into a v4l2loopback camera.
add_library(mycam_video STATIC companion/decoder_ffmpeg.cpp companion/frame_writer_v4l2.cpp vcam/frame_transform.cpp)
target_link_libraries(mycam_video PUBLIC PkgConfig::FFMPEG)
target_compile_options(mycam_video PRIVATE -Wall -Wextra)

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
