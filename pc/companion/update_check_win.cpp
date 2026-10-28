// The update check's HTTPS GET on Windows: WinHTTP, which follows the system proxy settings.
#include "update_check.h"

#include <windows.h>
#include <winhttp.h>

#include "platform.h"

namespace mycam {

namespace {
constexpr DWORD kTimeoutMs = 8000;
constexpr size_t kMaxBody = 256 * 1024; // A release reply is a few KB; anything bigger isn't one.
} // namespace

bool HttpsGet(const char* host, const char* path, std::string* body) {
    body->clear();
    // "MyCam" and nothing else: the GitHub API needs a User-Agent, and this sends no version or machine.
    HINTERNET session = WinHttpOpen(L"MyCam", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    bool ok = false;
    if (HINTERNET conn = WinHttpConnect(session, Wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(conn, L"GET", Wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
            if (WinHttpSendRequest(req, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    char buf[4096];
                    DWORD read = 0;
                    while (WinHttpReadData(req, buf, sizeof(buf), &read) && read) {
                        if (body->size() + read > kMaxBody) {
                            body->clear();
                            break;
                        }
                        body->append(buf, read);
                    }
                    ok = !body->empty();
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

} // namespace mycam
