// Wireless transport (IMPROVEMENTS.md 11, phase 1): finds MyCam phones on the local network and runs the
// normal protocol over TCP. See protocol/PROTOCOL.md "Wireless transport".
//
// The PC only ever sends (a UDP broadcast, then a TCP connect), so Windows never asks for a firewall rule:
// the phone's unicast answer to a broadcast is allowed back in as a response.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <string>
#include <vector>

#include "log.h"
#include "phone_link.h"

namespace mycam {

namespace {

constexpr uint16_t kDiscoveryPort = 47801;
constexpr char kAsk[] = "MYCAM?1";
constexpr char kHere[] = "MYCAM!1";
constexpr uint64_t kProbeEveryMs = 2000;
constexpr uint64_t kForgetAfterMs = 6000;      // A phone that stopped answering is gone.
constexpr uint64_t kRetryRefusedMs = 120000;   // The phone said no (or nobody answered): don't nag it.
constexpr uint64_t kRetryLostMs = 2000;        // The connection dropped after working: try again soon.
constexpr uint64_t kRetryUnreachableMs = 10000;

std::string ComputerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &n)) return "PC";
    char utf8[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, int(n), utf8, sizeof(utf8) - 1, nullptr, nullptr);
    return utf8;
}

// Directed broadcast address of every IPv4 network this PC is on (plus the generic one), so the probe
// goes out on Wi-Fi even when Ethernet or a VPN is the default route.
std::vector<uint32_t> BroadcastAddresses() {
    std::vector<uint32_t> out = {htonl(INADDR_BROADCAST)};
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const ULONG prefix = u->OnLinkPrefixLength;
            if (prefix == 0 || prefix >= 32) continue;
            const uint32_t mask = htonl(~0u << (32 - prefix));
            out.push_back(sin->sin_addr.s_addr | ~mask);
        }
    }
    return out;
}

// Connects with a timeout, then leaves the socket non-blocking.
SOCKET ConnectTcp(uint32_t ip, uint16_t port, int timeoutMs) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    // Large receive window: a 4K key frame can be several hundred KB.
    int rcv = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;
    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    fd_set wr, ex;
    FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(s, &wr); FD_SET(s, &ex);
    timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &wr, &ex, &tv) != 1 || !FD_ISSET(s, &wr)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

std::string IpText(uint32_t ip) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr a;
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, text, sizeof(text));
    return text;
}

} // namespace

bool PhoneLink::EnsureUdp() {
    if (udp_ != INVALID_SOCKET) return true;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sockaddr_in any = {};
    any.sin_family = AF_INET; // Any address, a free port.
    if (bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        closesocket(s);
        return false;
    }
    udp_ = s;
    return true;
}

void PhoneLink::BroadcastProbe() {
    const std::string message = std::string(kAsk) + " " + ComputerName();
    for (uint32_t addr : BroadcastAddresses()) {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(kDiscoveryPort);
        to.sin_addr.s_addr = addr;
        sendto(SOCKET(udp_), message.data(), int(message.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
}

void PhoneLink::NetScanOnce() {
    if (!wireless_) {
        if (udp_ != INVALID_SOCKET) {
            closesocket(SOCKET(udp_));
            udp_ = INVALID_SOCKET;
            netPhones_.clear();
        }
        return;
    }
    if (!EnsureUdp()) return;
    uint64_t now = GetTickCount64();
    if (now - lastProbe_ >= kProbeEveryMs) {
        lastProbe_ = now;
        BroadcastProbe();
    }

    // Answers: "MYCAM!1 <tcp port> <phone name>".
    char buf[512];
    for (;;) {
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int n = recvfrom(SOCKET(udp_), buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) break;
        buf[n] = 0;
        std::string text(buf, n);
        if (text.rfind(kHere, 0) != 0) continue;
        size_t portStart = text.find(' ');
        size_t nameStart = portStart == std::string::npos ? std::string::npos : text.find(' ', portStart + 1);
        int port = portStart == std::string::npos ? 0 : atoi(text.c_str() + portStart + 1);
        if (port <= 0 || port > 65535) continue;
        NetPhone& phone = netPhones_[from.sin_addr.s_addr];
        if (phone.lastSeen == 0) {
            phone.name = nameStart == std::string::npos ? IpText(from.sin_addr.s_addr) : text.substr(nameStart + 1);
            Log("wifi: found %s at %s:%d", phone.name.c_str(), IpText(from.sin_addr.s_addr).c_str(), port);
        }
        phone.port = uint16_t(port);
        phone.lastSeen = now;
    }

    // Connect to a phone that answered recently and isn't in back-off. The phone then asks its user.
    for (auto& [ip, phone] : netPhones_) {
        if (now - phone.lastSeen > kForgetAfterMs || now < phone.retryAfter) continue;
        SOCKET s = ConnectTcp(ip, phone.port, 2000);
        if (s == INVALID_SOCKET) {
            Log("wifi: can't connect to %s (%s:%u)", phone.name.c_str(), IpText(ip).c_str(), phone.port);
            phone.retryAfter = now + kRetryUnreachableMs;
            continue;
        }
        RunTcpSession(uintptr_t(s), phone.name);
        // No HELLO means the phone refused (or nobody tapped Allow): don't ask again for a while.
        phone.retryAfter = GetTickCount64() + (gotHello_ ? kRetryLostMs : kRetryRefusedMs);
        lastProbe_ = 0; // Look again right away.
        break;
    }
}

void PhoneLink::RunTcpSession(uintptr_t socket, const std::string& phoneName) {
    const SOCKET s = SOCKET(socket);
    tcp_ = socket;
    BeginSession(true, phoneName);
    Log("session: Wi-Fi connection to %s, waiting for the phone to allow it", phoneName.c_str());
    std::vector<uint8_t> buf(256 * 1024);
    uint64_t lastUsbCheck = GetTickCount64();
    uint64_t connected = lastUsbCheck;
    SessionLoop([&] {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv = {0, 100000};
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
            // Drain what has arrived, so a big frame isn't fed in 100 ms steps.
            for (int i = 0; i < 64; ++i) {
                int n = recv(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
                if (n > 0) { FeedBytes(buf.data(), size_t(n)); continue; }
                if (n == 0) Log("wifi: the phone closed the connection");
                else if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                else Log("wifi: connection lost (%d)", WSAGetLastError());
                sessionError_ = true;
                break;
            }
        }
        const uint64_t now = GetTickCount64();
        if (!wireless_) {
            Log("wifi: turned off on the PC");
            sessionError_ = true;
        } else if (!gotHello_ && now - connected > 65000) {
            Log("wifi: nobody allowed the connection on the phone");
            sessionError_ = true;
        } else if (now - lastUsbCheck > 2000) {
            lastUsbCheck = now;
            if (UsbPhoneArrived()) {
                Log("wifi: a phone was plugged in; switching to the cable");
                sessionError_ = true;
            }
        }
    });
    LeaveSession();
    shutdown(s, SD_BOTH);
    closesocket(s);
    tcp_ = INVALID_SOCKET;
    ResetAfterSession();
}

bool PhoneLink::SendTcp(const uint8_t* data, size_t size) {
    const SOCKET s = SOCKET(tcp_);
    size_t sent = 0;
    const uint64_t deadline = GetTickCount64() + 1000;
    while (sent < size) {
        int n = send(s, reinterpret_cast<const char*>(data + sent), int(size - sent), 0);
        if (n > 0) { sent += size_t(n); continue; }
        if (WSAGetLastError() != WSAEWOULDBLOCK || GetTickCount64() > deadline) {
            Log("send cmd failed over Wi-Fi (%d)", WSAGetLastError());
            return false;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv = {0, 50000};
        select(0, nullptr, &wr, nullptr, &tv);
    }
    return true;
}

} // namespace mycam
