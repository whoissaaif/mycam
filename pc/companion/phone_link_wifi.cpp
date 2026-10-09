// Wireless transport (IMPROVEMENTS.md 11): finds MyCam phones on the local network, pairs with them once
// (6-digit code compared on both screens), then runs the normal protocol over TCP inside AES-256-GCM
// records. See protocol/PROTOCOL.md "Wireless transport" and "Wireless security".
//
// The PC only ever sends (a UDP broadcast, then a TCP connect), so Windows never asks for a firewall rule:
// the phone's unicast answer to a broadcast is allowed back in as a response.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wincrypt.h>

#include <functional>
#include <string>
#include <vector>

#include "log.h"
#include "phone_link.h"
#include "wifi_crypto.h"

namespace mycam {

namespace {

using wifi::Bytes;

// --- Handshake messages: u16 length (type + body) | u8 type | body -----------------------------------
constexpr uint8_t kMsgClientHello = 1, kMsgServerHello = 2, kMsgNonceA = 3, kMsgNonceB = 4, kMsgPcFinished = 5,
                  kMsgPhoneFinished = 6, kMsgReject = 7;
constexpr uint8_t kModePaired = 0, kModePairing = 1;
constexpr uint8_t kFlagForcePair = 0x01;
constexpr uint8_t kRejectRefused = 1;
constexpr uint64_t kStepTimeoutMs = 10000;
constexpr uint64_t kUserTimeoutMs = 75000; // The phone gives its user 60 s.

// --- Paired phones: HKCU\Software\MyCam\PairedPhones\<phone id> = Key (DPAPI-protected), Name ----------
constexpr wchar_t kMyCamKey[] = L"Software\\MyCam";
constexpr wchar_t kPairedKey[] = L"Software\\MyCam\\PairedPhones";

std::wstring Wide(const std::string& utf8) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), int(w.size()));
    return w;
}

std::string Utf8(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::wstring HexId(const Bytes& id) {
    std::wstring s;
    wchar_t two[3];
    for (uint8_t b : id) { swprintf_s(two, L"%02x", b); s += two; }
    return s;
}

// This PC's random id, made once.
Bytes PcId() {
    Bytes id(16);
    DWORD size = DWORD(id.size()), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kMyCamKey, L"PcId", RRF_RT_REG_BINARY, &type, id.data(), &size) == ERROR_SUCCESS && size == 16)
        return id;
    id = wifi::Random(16);
    RegSetKeyValueW(HKEY_CURRENT_USER, kMyCamKey, L"PcId", REG_BINARY, id.data(), DWORD(id.size()));
    return id;
}

bool LoadPairKey(const Bytes& phoneId, Bytes* key) {
    const std::wstring sub = std::wstring(kPairedKey) + L"\\" + HexId(phoneId);
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), L"Key", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS) return false;
    Bytes blob(size);
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), L"Key", RRF_RT_REG_BINARY, nullptr, blob.data(), &size) != ERROR_SUCCESS) return false;
    DATA_BLOB in = {DWORD(blob.size()), blob.data()}, out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    key->assign(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return key->size() == 32;
}

void SavePairKey(const Bytes& phoneId, const Bytes& key, const std::string& name) {
    DATA_BLOB in = {DWORD(key.size()), const_cast<BYTE*>(key.data())}, out = {};
    if (!CryptProtectData(&in, L"MyCam phone pairing", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    const std::wstring sub = std::wstring(kPairedKey) + L"\\" + HexId(phoneId);
    RegSetKeyValueW(HKEY_CURRENT_USER, sub.c_str(), L"Key", REG_BINARY, out.pbData, out.cbData);
    const std::wstring wide = Wide(name);
    RegSetKeyValueW(HKEY_CURRENT_USER, sub.c_str(), L"Name", REG_SZ, wide.c_str(), DWORD((wide.size() + 1) * sizeof(wchar_t)));
    LocalFree(out.pbData);
}

void ForgetPairKey(const Bytes& phoneId) {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(kPairedKey) + L"\\" + HexId(phoneId)).c_str());
}

// --- Socket helpers (the socket is non-blocking) --------------------------------------------------

bool SendAll(SOCKET s, const uint8_t* data, size_t size, uint64_t timeoutMs = 1000) {
    size_t sent = 0;
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    while (sent < size) {
        int n = send(s, reinterpret_cast<const char*>(data + sent), int(size - sent), 0);
        if (n > 0) { sent += size_t(n); continue; }
        if (WSAGetLastError() != WSAEWOULDBLOCK || GetTickCount64() > deadline) return false;
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv = {0, 50000};
        select(0, nullptr, &wr, nullptr, &tv);
    }
    return true;
}

// Reads exactly `size` bytes, giving up after `timeoutMs` or when `keepWaiting` says so (it runs about
// every 100 ms, so the status pictures keep updating while the phone's user decides).
bool RecvExact(SOCKET s, uint8_t* out, size_t size, uint64_t timeoutMs, const std::function<bool()>& keepWaiting) {
    size_t got = 0;
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    while (got < size) {
        int n = recv(s, reinterpret_cast<char*>(out + got), int(size - got), 0);
        if (n > 0) { got += size_t(n); continue; }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return false;
        if (GetTickCount64() > deadline || !keepWaiting()) return false;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv = {0, 100000};
        select(0, &rd, nullptr, nullptr, &tv);
    }
    return true;
}

Bytes Frame(uint8_t type, const Bytes& body) {
    Bytes f = {uint8_t((body.size() + 1) >> 8), uint8_t(body.size() + 1), type};
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

Bytes NameField(const std::string& name) {
    Bytes b = {uint8_t(std::min<size_t>(name.size(), 64))};
    b.insert(b.end(), name.begin(), name.begin() + b[0]);
    return b;
}

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

    // Connect to a phone that answered recently and isn't in back-off.
    for (auto& [ip, phone] : netPhones_) {
        if (now - phone.lastSeen > kForgetAfterMs || now < phone.retryAfter) continue;
        SOCKET s = ConnectTcp(ip, phone.port, 2000);
        if (s == INVALID_SOCKET) {
            Log("wifi: can't connect to %s (%s:%u)", phone.name.c_str(), IpText(ip).c_str(), phone.port);
            phone.retryAfter = now + kRetryUnreachableMs;
            continue;
        }
        const bool forcePair = phone.forcePair;
        phone.forcePair = false;
        switch (RunTcpSession(uintptr_t(s), phone.name, forcePair)) {
        case WifiEnd::Ran: phone.retryAfter = GetTickCount64() + kRetryLostMs; break;
        // The user said no (or nobody answered): don't ask again for a while.
        case WifiEnd::Refused: phone.retryAfter = GetTickCount64() + kRetryRefusedMs; break;
        // Our pairing key doesn't work any more (e.g. this PC forgot the phone): pair again, with a code.
        case WifiEnd::NeedsPairing: phone.forcePair = true; phone.retryAfter = 0; break;
        case WifiEnd::Failed: phone.retryAfter = GetTickCount64() + kRetryUnreachableMs; break;
        }
        lastProbe_ = 0; // Look again right away.
        break;
    }
}

// PROTOCOL.md "Wireless security". A paired phone is proven with the stored pairing key and connects
// without asking. A new phone is paired: the phone commits to its nonce before seeing ours, both sides
// show the same 6-digit code, and the user allows it on the phone. Either way the session keys come from
// a fresh ECDH, so earlier sessions stay safe if a key ever leaks.
PhoneLink::WifiEnd PhoneLink::Handshake(uintptr_t socket, bool forcePair) {
    const SOCKET s = SOCKET(socket);
    auto keepWaiting = [this] {
        const uint64_t now = GetTickCount64();
        SyncLockPaused();
        WriteStatusFrame(now);
        return !quit_ && wireless_;
    };
    Bytes transcript;
    auto sendMsg = [&](uint8_t type, const Bytes& body, bool record) {
        const Bytes f = Frame(type, body);
        if (record) transcript.insert(transcript.end(), f.begin(), f.end());
        return SendAll(s, f.data(), f.size());
    };
    // Returns the body, or false (refused / wrong message / timeout).
    auto readMsg = [&](uint8_t expected, Bytes* body, uint64_t timeoutMs, bool record, bool* rejected) {
        uint8_t head[3];
        if (!RecvExact(s, head, 3, timeoutMs, keepWaiting)) return false;
        const size_t len = size_t(head[0]) << 8 | head[1];
        if (len < 1) return false;
        body->resize(len - 1);
        if (!body->empty() && !RecvExact(s, body->data(), body->size(), kStepTimeoutMs, keepWaiting)) return false;
        if (head[2] == kMsgReject) {
            *rejected = true;
            Log("wifi: the phone refused (reason %d)", body->empty() ? 0 : int((*body)[0]));
            return false;
        }
        if (head[2] != expected) { Log("wifi: unexpected handshake message %d", int(head[2])); return false; }
        if (record) {
            transcript.insert(transcript.end(), head, head + 3);
            transcript.insert(transcript.end(), body->begin(), body->end());
        }
        return true;
    };

    wifi::EcKey key;
    if (!key.Generate()) return WifiEnd::Failed;
    const Bytes pcPub = key.PublicRaw(), npc = wifi::Random(16);
    Bytes hello = wifi::Ascii("MCHS");
    hello.push_back(1);                              // Version
    hello.push_back(forcePair ? kFlagForcePair : 0); // Flags
    const Bytes pcId = PcId();
    const Bytes name = NameField(ComputerName());
    hello = wifi::Concat({&hello, &pcId, &pcPub, &npc, &name});
    if (!sendMsg(kMsgClientHello, hello, true)) return WifiEnd::Failed;

    bool rejected = false;
    Bytes sh;
    if (!readMsg(kMsgServerHello, &sh, kStepTimeoutMs, true, &rejected)) return rejected ? WifiEnd::Refused : WifiEnd::Failed;
    if (sh.size() < 16 + 65 + 16 + 1 + 1) return WifiEnd::Failed;
    const Bytes phoneId(sh.begin(), sh.begin() + 16), phonePub(sh.begin() + 16, sh.begin() + 81),
                nph(sh.begin() + 81, sh.begin() + 97);
    const uint8_t mode = sh[97];
    Bytes z;
    if (!key.Agree(phonePub, &z)) return WifiEnd::Failed;

    Bytes pairKey, keys;
    if (mode == kModePaired) {
        if (!LoadPairKey(phoneId, &pairKey)) {
            Log("wifi: the phone remembers this PC, but this PC forgot it: pairing again");
            uint8_t reject[] = {0, 2, kMsgReject, 2};
            SendAll(s, reject, sizeof(reject));
            return WifiEnd::NeedsPairing;
        }
        keys = wifi::SessionKeys(z, pairKey, npc, nph);
    } else if (mode == kModePairing && sh.size() >= 98 + 32 + 1) {
        const Bytes commit(sh.begin() + 98, sh.begin() + 130);
        const Bytes na = wifi::Random(16);
        if (!sendMsg(kMsgNonceA, na, true)) return WifiEnd::Failed;
        Bytes nb;
        if (!readMsg(kMsgNonceB, &nb, kStepTimeoutMs, true, &rejected) || nb.size() != 16)
            return rejected ? WifiEnd::Refused : WifiEnd::Failed;
        if (wifi::Commit(nb, phonePub, pcPub) != commit) {
            Log("wifi: the phone's commitment doesn't match: someone may be in between; not pairing");
            return WifiEnd::Failed;
        }
        pairKey = wifi::PairKey(z, na, nb);
        keys = wifi::SessionKeys(z, pairKey, npc, nph);
        const std::string code = wifi::Sas(pcPub, phonePub, na, nb);
        status_.pairingCode = std::wstring(code.begin(), code.begin() + 3) + L" " + std::wstring(code.begin() + 3, code.end());
        Publish();
        Log("wifi: pairing with %s, code %s", Utf8(status_.phoneName).c_str(), code.c_str());
    } else {
        return WifiEnd::Failed;
    }

    const Bytes h = wifi::Sha256(transcript);
    const Bytes kFin(keys.begin() + 64, keys.end());
    if (!sendMsg(kMsgPcFinished, wifi::Finished(kFin, "PC", h), false)) return WifiEnd::Failed;
    Bytes phoneFinished;
    const bool ok = readMsg(kMsgPhoneFinished, &phoneFinished, mode == kModePairing ? kUserTimeoutMs : kStepTimeoutMs, false, &rejected);
    status_.pairingCode.clear();
    Publish();
    if (!ok) {
        // A paired phone that rejects our proof has a different key for us (it re-paired, or forgot us).
        if (rejected && mode == kModePaired) { ForgetPairKey(phoneId); return WifiEnd::NeedsPairing; }
        return rejected ? WifiEnd::Refused : WifiEnd::Failed;
    }
    if (phoneFinished != wifi::Finished(kFin, "PH", h)) {
        Log("wifi: the phone's proof is wrong; not connecting");
        return WifiEnd::Failed;
    }
    if (mode == kModePairing) {
        const std::string phoneName = Utf8(status_.phoneName);
        SavePairKey(phoneId, pairKey, phoneName);
        Log("wifi: paired");
    }
    wifiTx_ = std::make_shared<wifi::RecordKey>();
    wifiRx_ = std::make_shared<wifi::RecordKey>();
    wifiTx_->Init(Bytes(keys.begin(), keys.begin() + 32), wifi::kDirPcToPhone);
    wifiRx_->Init(Bytes(keys.begin() + 32, keys.begin() + 64), wifi::kDirPhoneToPc);
    wifiRxBuf_.clear();
    return WifiEnd::Ran;
}

PhoneLink::WifiEnd PhoneLink::RunTcpSession(uintptr_t socket, const std::string& phoneName, bool forcePair) {
    const SOCKET s = SOCKET(socket);
    BeginSession(true, phoneName);
    Log("session: Wi-Fi connection to %s%s", phoneName.c_str(), forcePair ? " (pairing again)" : "");
    WifiEnd end = Handshake(socket, forcePair);
    if (end == WifiEnd::Ran) {
        Log("session: Wi-Fi link to %s is encrypted", phoneName.c_str());
        tcp_ = socket;
        uint64_t lastUsbCheck = GetTickCount64();
        SessionLoop([&] {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv = {0, 100000};
            if (select(0, &rd, nullptr, nullptr, &tv) > 0 && !ReceiveRecords(socket)) sessionError_ = true;
            const uint64_t now = GetTickCount64();
            if (!wireless_) {
                Log("wifi: turned off on the PC");
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
    }
    shutdown(s, SD_BOTH);
    closesocket(s);
    tcp_ = INVALID_SOCKET;
    wifiTx_.reset();
    wifiRx_.reset();
    wifiRxBuf_.clear();
    ResetAfterSession();
    return end;
}

// Drains the socket into wifiRxBuf_, then decrypts every complete record into the protocol parser.
bool PhoneLink::ReceiveRecords(uintptr_t socket) {
    const SOCKET s = SOCKET(socket);
    uint8_t buf[64 * 1024];
    for (int i = 0; i < 64; ++i) {
        int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) { wifiRxBuf_.insert(wifiRxBuf_.end(), buf, buf + n); continue; }
        if (n == 0) { Log("wifi: the phone closed the connection"); return false; }
        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
        Log("wifi: connection lost (%d)", WSAGetLastError());
        return false;
    }
    size_t pos = 0;
    Bytes plain;
    while (wifiRxBuf_.size() - pos >= 4) {
        const uint8_t* p = wifiRxBuf_.data() + pos;
        const size_t len = size_t(p[0]) << 24 | size_t(p[1]) << 16 | size_t(p[2]) << 8 | p[3];
        if (len < 16 || len > wifi::kMaxRecord) { Log("wifi: bad record length"); return false; }
        if (wifiRxBuf_.size() - pos - 4 < len) break; // Wait for the rest.
        if (!wifiRx_->Open(p + 4, len, &plain)) { Log("wifi: a record failed its check; dropping the connection"); return false; }
        pos += 4 + len;
        FeedBytes(plain.data(), plain.size());
    }
    wifiRxBuf_.erase(wifiRxBuf_.begin(), wifiRxBuf_.begin() + pos);
    return true;
}

bool PhoneLink::SendTcp(const uint8_t* data, size_t size) {
    Bytes record;
    if (!wifiTx_ || !wifiTx_->Seal(data, size, &record)) return false;
    if (!SendAll(SOCKET(tcp_), record.data(), record.size())) {
        Log("send cmd failed over Wi-Fi (%d)", WSAGetLastError());
        return false;
    }
    return true;
}

void ForgetPairedPhones() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kPairedKey);
}

} // namespace mycam
