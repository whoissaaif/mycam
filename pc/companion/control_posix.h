#pragma once
// Linux: talking to the running daemon. It listens on $XDG_RUNTIME_DIR/mycam.sock (this user only), so
// `mycam-companion --pair` in any terminal can show the iPhone QR code while the service runs: the Windows
// settings window's "Connect an iPhone", for a daemon with no window.
//
// Lines over the socket. Client: "pair". Daemon: "url <pairing address>" whenever it changes (empty: none
// yet), "paired <name>" when a device pairs. Closing the connection stops pairing. The address holds the
// one-time code, so it only ever goes over this socket, never to the log.

#include <stdint.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "link_status.h"

namespace mycam {

class PhoneLink;

class ControlServer {
public:
    explicit ControlServer(PhoneLink* link) : link_(link) {}
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ~ControlServer() { Stop(); }

    bool Start();  // Logs why not (another daemon already owns the socket, no runtime dir...).
    void Stop();
    void OnStatus(const LinkStatus& status); // From PhoneLink's status callback.

private:
    void Loop();
    void Serve(int client);

    PhoneLink* link_;
    int listen_ = -1;
    std::string path_;
    std::thread thread_;
    bool stop_ = false;           // Guarded by lock_.
    std::mutex lock_;
    std::condition_variable changed_;
    std::string url_;             // Guarded by lock_.
    uint32_t pairedCount_ = 0;    // Guarded by lock_.
    std::string pairedName_;      // Guarded by lock_.
};

// `mycam-companion --pair` without --daemon: asks the running daemon to pair and prints its QR codes until a
// device pairs or Ctrl+C. Returns the process exit code.
int RunPairClient(void (*printQr)(const std::string& url));

} // namespace mycam
