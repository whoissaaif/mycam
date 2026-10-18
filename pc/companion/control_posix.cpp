#include "control_posix.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"
#include "phone_link.h"
#include "platform.h"

namespace mycam {

namespace {

std::string SocketPath() {
    const char* dir = getenv("XDG_RUNTIME_DIR"); // Per user, 0700, made by systemd at login.
    return dir && *dir ? std::string(dir) + "/mycam.sock" : std::string();
}

int Connect(const std::string& path) {
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return -1;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

bool SendLine(int s, const std::string& line) {
    const std::string text = line + "\n";
    return send(s, text.data(), text.size(), MSG_NOSIGNAL) == ssize_t(text.size());
}

// One line (without the newline) into *line; false when the connection ended. *buffer keeps what follows.
bool ReadLine(int s, std::string* buffer, std::string* line, int timeoutMs) {
    for (;;) {
        if (const size_t nl = buffer->find('\n'); nl != std::string::npos) {
            *line = buffer->substr(0, nl);
            buffer->erase(0, nl + 1);
            return true;
        }
        pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, timeoutMs) <= 0) return false;
        char chunk[512];
        const ssize_t n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0 || buffer->size() > 4096) return false;
        buffer->append(chunk, size_t(n));
    }
}

} // namespace

bool ControlServer::Start() {
    path_ = SocketPath();
    if (path_.empty()) {
        Log("control: no XDG_RUNTIME_DIR, so mycam-companion --pair can't reach this daemon");
        return false;
    }
    if (const int other = Connect(path_); other >= 0) {
        close(other);
        Log("control: another mycam-companion already answers on %s", path_.c_str());
        path_.clear();
        return false;
    }
    unlink(path_.c_str()); // Left over from a daemon that didn't exit cleanly.
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) return false;
    memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    listen_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_ < 0 || bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_, 2) != 0) {
        Log("control: can't listen on %s: %s", path_.c_str(), strerror(errno));
        if (listen_ >= 0) close(listen_);
        listen_ = -1;
        path_.clear();
        return false;
    }
    chmod(path_.c_str(), 0600);
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void ControlServer::Stop() {
    {
        std::lock_guard<std::mutex> lock(lock_);
        stop_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (listen_ >= 0) close(listen_);
    listen_ = -1;
    if (!path_.empty()) unlink(path_.c_str());
    path_.clear();
}

void ControlServer::OnStatus(const LinkStatus& status) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        url_ = Utf8(status.webUrl);
        if (status.pairedCount != pairedCount_) pairedName_ = Utf8(status.pairedName);
        pairedCount_ = status.pairedCount;
    }
    changed_.notify_all();
}

void ControlServer::Loop() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (stop_) return;
        }
        pollfd p = {listen_, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;
        const int client = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        Serve(client); // One at a time; a second terminal waits until the first lets go.
        close(client);
    }
}

void ControlServer::Serve(int client) {
    std::string buffer, command;
    if (!ReadLine(client, &buffer, &command, 2000) || command != "pair") {
        SendLine(client, "error unknown command");
        return;
    }
    Log("control: pairing asked for from a terminal");
    link_->StartWebPairing(0);
    std::unique_lock<std::mutex> lock(lock_);
    const uint32_t pairedBefore = pairedCount_;
    std::string sent = "\n"; // Not a possible address: the first one always goes out.
    uint32_t pairedSeen = pairedBefore;
    while (!stop_) {
        if (url_ != sent) {
            sent = url_;
            if (!SendLine(client, "url " + sent)) break;
        }
        if (pairedCount_ != pairedSeen) {
            pairedSeen = pairedCount_;
            if (!SendLine(client, "paired " + pairedName_)) break;
        }
        changed_.wait_for(lock, std::chrono::milliseconds(500));
        pollfd p = {client, POLLIN, 0};
        if (poll(&p, 1, 0) > 0) break; // The client hung up (it never sends more).
    }
    lock.unlock();
    link_->StopWebPairing();
    Log("control: pairing from the terminal ended");
}

int RunPairClient(void (*printQr)(const std::string& url)) {
    const int s = Connect(SocketPath());
    if (s < 0) {
        fprintf(stderr, "MyCam isn't running. Start it with: systemctl --user start mycam\n"
                        "(or run mycam-companion --daemon --pair)\n");
        return 1;
    }
    if (!SendLine(s, "pair")) { close(s); return 1; }
    std::string buffer, line;
    while (ReadLine(s, &buffer, &line, -1)) {
        if (line.rfind("url ", 0) == 0) {
            const std::string url = line.substr(4);
            if (url.empty()) printf("Waiting for a network address a phone could reach...\n");
            else printQr(url);
            fflush(stdout);
        } else if (line.rfind("paired ", 0) == 0) {
            printf("Paired with %s. It can connect any time now; this window can close.\n", line.substr(7).c_str());
            close(s);
            return 0;
        } else if (line.rfind("error ", 0) == 0) {
            fprintf(stderr, "MyCam: %s\n", line.substr(6).c_str());
            break;
        }
    }
    close(s);
    fprintf(stderr, "MyCam stopped answering.\n");
    return 1;
}

} // namespace mycam
