#include "usbmux_transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "log.h"

namespace mycam {

namespace {

constexpr uint16_t kIosPort = 5000;
constexpr uint16_t kUsbmuxPort = 27015;
constexpr uintptr_t kInvalidSocket = UINTPTR_MAX;

SOCKET ToSocket(UsbmuxTransport::SocketHandle s) { return SOCKET(s); }

} // namespace

UsbmuxTransport::~UsbmuxTransport() {
    Close();
    if (wsaReady_) WSACleanup();
}

void UsbmuxTransport::Emit(TransportEvent event) {
    if (onEvent_) onEvent_(event);
}

bool UsbmuxTransport::EnsureWinsock() {
    if (wsaReady_) return true;
    WSADATA wsa = {};
    wsaReady_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    return wsaReady_;
}

UsbmuxTransport::SocketHandle UsbmuxTransport::OpenUsbmuxSocket() {
    if (!EnsureWinsock()) return kInvalidSocket;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return kInvalidSocket;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kUsbmuxPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return kInvalidSocket;
    }
    int timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return SocketHandle(s);
}

bool UsbmuxTransport::SendAll(SocketHandle socket, const uint8_t* data, size_t size) {
    SOCKET s = ToSocket(socket);
    size_t sent = 0;
    while (sent < size) {
        int n = send(s, reinterpret_cast<const char*>(data + sent), int(size - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool UsbmuxTransport::RecvPackets(SocketHandle socket, usbmux::PacketReader* reader, const usbmux::PacketReader::Callback& cb) {
    uint8_t buf[4096];
    int n = recv(ToSocket(socket), reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (n > 0) {
        reader->Feed(buf, size_t(n), cb);
        return true;
    }
    int err = WSAGetLastError();
    return n < 0 && (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK);
}

bool UsbmuxTransport::SendPacket(SocketHandle s, const usbmux::Bytes& packet) {
    return SendAll(s, packet.data(), packet.size());
}

bool UsbmuxTransport::ReadResult(SocketHandle s, uint32_t tag, uint32_t* number) {
    usbmux::PacketReader reader;
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i) {
        if (!RecvPackets(s, &reader, [&](const usbmux::Header& h, const std::string& payload) {
            if (h.tag != tag) return;
            usbmux::Message msg;
            if (usbmux::ParseMessage(payload, &msg) && msg.messageType == "Result") {
                *number = msg.number;
                got = true;
            }
        })) {
            return false;
        }
    }
    return got;
}

UsbmuxTransport::SocketHandle UsbmuxTransport::ConnectToDevice(uint32_t deviceId) {
    SocketHandle s = OpenUsbmuxSocket();
    if (s == kInvalidSocket) return kInvalidSocket;
    uint32_t tag = nextTag_++;
    if (!SendPacket(s, usbmux::BuildConnectPacket(tag, deviceId, kIosPort))) {
        closesocket(ToSocket(s));
        return kInvalidSocket;
    }
    uint32_t result = 0xFFFFFFFF;
    if (!ReadResult(s, tag, &result) || result != 0) {
        closesocket(ToSocket(s));
        return kInvalidSocket;
    }
    // The 200 ms receive timeout is only for the polling Listen socket and the
    // Result handshake. The tunnel's reader thread must block until data or close,
    // otherwise an idle phone looks like a dropped connection.
    int noTimeout = 0;
    setsockopt(ToSocket(s), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    return s;
}

bool UsbmuxTransport::Connect(std::atomic<bool>& quit) {
    while (!quit.load()) {
        SocketHandle listen = OpenUsbmuxSocket();
        if (listen == kInvalidSocket) {
            if (!loggedUnavailable_) {
                Log("usbmuxd not available: install iTunes or the Apple Devices app to use iPhones");
                loggedUnavailable_ = true;
            }
            for (int i = 0; i < 30 && !quit.load(); ++i) Sleep(100);
            continue;
        }

        uint32_t listenTag = nextTag_++;
        bool listening = SendPacket(listen, usbmux::BuildListenPacket(listenTag));
        uint32_t listenResult = 0xFFFFFFFF;
        if (listening) listening = ReadResult(listen, listenTag, &listenResult) && listenResult == 0;
        if (!listening) {
            closesocket(ToSocket(listen));
            for (int i = 0; i < 30 && !quit.load(); ++i) Sleep(100);
            continue;
        }

        std::map<uint32_t, usbmux::Message> devices;
        usbmux::PacketReader reader;
        uint64_t lastConnectTry = 0;
        while (!quit.load()) {
            if (!RecvPackets(listen, &reader, [&](const usbmux::Header&, const std::string& payload) {
                usbmux::Message msg;
                if (!usbmux::ParseMessage(payload, &msg)) return;
                if (msg.messageType == "Attached" && msg.connectionType == "USB") {
                    devices[msg.deviceId] = msg;
                    Log("usbmux: iPhone attached id=%u serial=%s", msg.deviceId, msg.serialNumber.c_str());
                } else if (msg.messageType == "Detached") {
                    devices.erase(msg.deviceId);
                    Log("usbmux: iPhone detached id=%u", msg.deviceId);
                    if (msg.deviceId == activeDeviceId_) CloseTunnel(true);
                }
            })) {
                break;
            }

            if (reconnect_.exchange(false)) CloseTunnel(true);

            bool active = false;
            {
                std::lock_guard<std::mutex> lock(socketLock_);
                active = tunnel_ != kInvalidSocket;
            }
            if (!active && !devices.empty()) {
                uint64_t now = GetTickCount64();
                if (lastConnectTry == 0 || now - lastConnectTry >= 2000) {
                    lastConnectTry = now;
                    uint32_t id = devices.begin()->first;
                    SocketHandle tunnel = ConnectToDevice(id);
                    if (tunnel == kInvalidSocket) {
                        Emit(TransportEvent::Waiting);
                    } else {
                        StartTunnel(tunnel, id);
                    }
                }
            }

            Sleep(50);
        }
        closesocket(ToSocket(listen));
    }
    return false;
}

void UsbmuxTransport::StartTunnel(SocketHandle s, uint32_t deviceId) {
    if (readerThread_.joinable()) readerThread_.join();
    {
        std::lock_guard<std::mutex> lock(socketLock_);
        tunnel_ = s;
        activeDeviceId_ = deviceId;
    }
    Log("usbmux: connected to iPhone id=%u port=%u", deviceId, kIosPort);
    Emit(TransportEvent::Connected);
    readerThread_ = std::thread([this, s] { ReaderLoop(s); });
}

void UsbmuxTransport::ReaderLoop(SocketHandle socket) {
    uint8_t buf[16384];
    int n = 0;
    while (true) {
        n = recv(ToSocket(socket), reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) {
            if (onData_) onData_(buf, size_t(n));
            continue;
        }
        if (n < 0) {
            int err = WSAGetLastError();
            // A timeout is not a closed connection; keep waiting for the phone.
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
        }
        break;
    }
    Log("usbmux: tunnel read ended (recv=%d, wsa error %d)", n, WSAGetLastError());
    CloseTunnel(false);
}

bool UsbmuxTransport::Write(const uint8_t* data, size_t size, uint32_t timeoutMs) {
    SocketHandle socket;
    {
        std::lock_guard<std::mutex> lock(socketLock_);
        socket = tunnel_;
    }
    if (socket == kInvalidSocket) return false;
    int timeout = int(timeoutMs);
    setsockopt(ToSocket(socket), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return SendAll(socket, data, size);
}

void UsbmuxTransport::CloseTunnel(bool joinReader) {
    SocketHandle socket = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(socketLock_);
        socket = tunnel_;
        tunnel_ = kInvalidSocket;
        activeDeviceId_ = 0;
    }
    if (socket != kInvalidSocket) {
        shutdown(ToSocket(socket), SD_BOTH);
        closesocket(ToSocket(socket));
        Emit(TransportEvent::Disconnected);
    }
    if (joinReader && readerThread_.joinable() && readerThread_.get_id() != std::this_thread::get_id()) readerThread_.join();
}

void UsbmuxTransport::Close() {
    CloseTunnel(true);
}

void UsbmuxTransport::RequestReconnect() {
    reconnect_ = true;
    CloseTunnel(true);
}

} // namespace mycam