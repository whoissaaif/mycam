// The update check's HTTPS GET on Linux: OpenSSL, which the companion already links for the iPhone page.
#include "update_check.h"

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "log.h"

namespace mycam {

namespace {

constexpr size_t kMaxBody = 256 * 1024; // A release reply is a few KB; anything bigger isn't one.
constexpr int kTimeoutSec = 8;

// A TCP connection to host:443, or -1. Blocking, with a receive/send timeout so a silent server can't
// wedge the thread.
int ConnectTo(const char* host) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host, "443", &hints, &list) != 0 || !list) return -1;
    int fd = -1;
    for (addrinfo* a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        const timeval tv = {kTimeoutSec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

// The body after the blank line, if the status line says 200. "" otherwise. The request asks for HTTP/1.0,
// so the body is never chunk-framed and needs no unwrapping.
std::string BodyOf200(const std::string& reply) {
    if (reply.compare(0, 9, "HTTP/1.1 ") != 0 && reply.compare(0, 9, "HTTP/1.0 ") != 0) return "";
    if (reply.compare(9, 3, "200") != 0) return "";
    const size_t blank = reply.find("\r\n\r\n");
    if (blank == std::string::npos) return "";
    return reply.substr(blank + 4);
}

} // namespace

bool HttpsGet(const char* host, const char* path, std::string* body) {
    body->clear();
    const int fd = ConnectTo(host);
    if (fd < 0) return false;
    bool ok = false;
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = nullptr;
    if (ctx) {
        // The distro's CA bundle, and the name must match: this is the one place MyCam talks to the
        // internet, so an unverified reply is no better than no check at all.
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        ssl = SSL_new(ctx);
    }
    if (ssl && SSL_set_tlsext_host_name(ssl, host) == 1 && SSL_set1_host(ssl, host) == 1 &&
        SSL_set_fd(ssl, fd) == 1 && SSL_connect(ssl) == 1) {
        // "MyCam" and nothing else: the GitHub API needs a User-Agent, and this sends no version or machine.
        const std::string request = std::string("GET ") + path + " HTTP/1.0\r\nHost: " + host +
                                    "\r\nUser-Agent: MyCam\r\nAccept: application/vnd.github+json\r\n"
                                    "Connection: close\r\n\r\n";
        if (SSL_write(ssl, request.data(), int(request.size())) == int(request.size())) {
            std::string reply;
            char buf[4096];
            int n;
            while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
                if (reply.size() + size_t(n) > kMaxBody) {
                    reply.clear();
                    break;
                }
                reply.append(buf, size_t(n));
            }
            *body = BodyOf200(reply);
            ok = !body->empty();
        }
        SSL_shutdown(ssl);
    } else if (ssl) {
        Log("update check: TLS to %s failed", host);
    }
    ERR_clear_error();
    if (ssl) SSL_free(ssl);
    if (ctx) SSL_CTX_free(ctx);
    close(fd);
    return ok;
}

} // namespace mycam
