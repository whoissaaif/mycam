# Linux build (docs/LINUX-SUPPORT.md). Included from CMakeLists.txt when not on Windows. For now only the
# unit tests: the portable companion code, with OpenSSL standing in for Windows CNG.

find_package(OpenSSL 3 REQUIRED)

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
    # The iPhone page's protocol code against the same golden vectors (needs Node.js; skipped without it).
    find_program(NODE_EXE node)
    if(NODE_EXE)
        add_test(NAME web_golden COMMAND ${NODE_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/webtest/golden_check.js
                 ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt)
    endif()
endif()
