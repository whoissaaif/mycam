// The iPhone web camera's HTTPS + WebSocket server. See web_server.h and docs/web-camera-format.md.
//
// Routes:
//   GET  /, /app.js, /protocol.js, /style.css   the page, from the exe's resources (companion.rc)
//   POST /pair    t=<one-time code>&name=<device>  -> {"device": "<device token>", "name": "..."}
//   POST /check   d=<device token>                 -> {"ok": true, "busy": false, "name": "..."}
//   GET  /ws?d=<device token>                      WebSocket: one MyCam packet or command per message
// Tokens never reach the log.

#define FD_SETSIZE 128
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "web_server.h"

#include <time.h>

#include <algorithm>

#include "log.h"
#include "tls_server.h"
#include "wifi_crypto.h"

namespace mycam {

namespace {

constexpr size_t kMaxConns = 48;
constexpr uint64_t kHandshakeMs = 10000;   // TLS + first request.
constexpr uint64_t kHttpIdleMs = 30000;    // Keep-alive connections between requests.
constexpr uint64_t kPhoneIdleMs = 15000;   // The page answers pings, so silence means it's gone.
constexpr uint64_t kPingEveryMs = 5000;
constexpr size_t kMaxPending = 4 * 1024 * 1024; // Unsent bytes before a reader counts as stuck.
constexpr size_t kMaxEarly = 256 * 1024;   // What a page may send before its session starts.
constexpr uint16_t kCloseBusy = 4001;      // Another phone is using MyCam: try again later.
constexpr uint16_t kCloseReplaced = 4002;  // The same device opened a newer connection.
constexpr uint16_t kCloseForgotten = 4003; // The PC forgot this device: scan the QR code again.
constexpr wchar_t kDevicesKey[] = L"Software\\MyCam\\WebDevices";

// Page files embedded in companion.rc. IDs must match there.
struct PageFile { const char* path; int id; const char* type; };
constexpr PageFile kFiles[] = {
    {"/", 301, "text/html; charset=utf-8"},
    {"/index.html", 301, "text/html; charset=utf-8"},
    {"/app.js", 302, "text/javascript; charset=utf-8"},
    {"/protocol.js", 303, "text/javascript; charset=utf-8"},
    {"/style.css", 304, "text/css; charset=utf-8"},
    {"/icon.png", 305, "image/png"},
};

std::string Resource(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* p = g ? LockResource(g) : nullptr;
    return p ? std::string(static_cast<const char*>(p), SizeofResource(nullptr, r)) : std::string();
}

std::string IpText(uint32_t ip) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr a;
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, text, sizeof(text));
    return text;
}

std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') { out += '\\'; out += ch; }
        else if (c < 0x20) { char esc[8]; sprintf_s(esc, "\\u%04x", c); out += esc; }
        else out += ch;
    }
    return out + "\"";
}

std::wstring Wide(const std::string& utf8) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), int(w.size()));
    return w;
}

std::string Utf8(const wchar_t* w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), int(s.size()), nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

} // namespace

struct WebServer::Conn {
    enum class Mode { Http, PhonePending, Phone };
    SOCKET s = INVALID_SOCKET;
    std::unique_ptr<tls::Session> tls;
    Mode mode = Mode::Http;
    web::Bytes in;          // Decrypted request bytes not handled yet (HTTP mode).
    web::Bytes out;         // Encrypted bytes not sent yet.
    size_t outPos = 0;
    web::WsParser ws;
    web::Bytes early;       // Messages from a waiting page, framed back to back (MyCam packets need no more).
    std::string deviceId, deviceName, peer;
    uint32_t localIp = 0;
    uint64_t opened = 0, lastIn = 0;
    bool closeAfterSend = false;
    bool closeSent = false;
    bool dead = false;
};

WebServer::WebServer() : listen_(INVALID_SOCKET) {}

WebServer::~WebServer() {
    for (auto& c : conns_) if (!c->dead) closesocket(c->s);
    if (listen_ != INVALID_SOCKET) closesocket(SOCKET(listen_));
}

bool WebServer::Listening() const { return listen_ != INVALID_SOCKET; }

bool WebServer::Start() {
    LoadDevices();
    cred_ = std::make_unique<tls::Credentials>();
    std::string why;
    if (!cred_->Init(&why)) {
        Log("web: no certificate, so no iPhone page: %s", why.c_str());
        cred_.reset();
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL exclusive = TRUE; // Nobody else may take the port from under us.
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(web::kPort);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        Log("web: can't listen on port %u (%d): another program may be using it", unsigned(web::kPort), WSAGetLastError());
        closesocket(s);
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    listen_ = s;
    RefreshAddresses();
    Log("web: iPhone page listening on port %u (certificate %s), %u remembered device(s)", unsigned(web::kPort),
        cred_->Thumbprint().c_str(), unsigned(book_.Devices().size()));
    return true;
}

void WebServer::Poll(uint32_t timeoutMs) {
    if (listen_ == INVALID_SOCKET) {
        if (timeoutMs) Sleep(timeoutMs);
        return;
    }
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_SET(SOCKET(listen_), &rd);
    for (auto& c : conns_) {
        if (c->dead) continue;
        FD_SET(c->s, &rd);
        if (c->outPos < c->out.size()) FD_SET(c->s, &wr);
    }
    timeval tv = {long(timeoutMs / 1000), long(timeoutMs % 1000) * 1000};
    if (select(0, &rd, &wr, nullptr, &tv) > 0) {
        if (FD_ISSET(SOCKET(listen_), &rd)) Accept();
        for (size_t i = 0; i < conns_.size(); ++i) {
            Conn& c = *conns_[i];
            if (!c.dead && FD_ISSET(c.s, &rd)) ReadFrom(c);
            if (!c.dead && FD_ISSET(c.s, &wr)) Flush(c);
        }
    }
    // Messages a page sent while it waited for its session.
    for (auto& c : conns_) {
        if (!c->dead && c->mode == Conn::Mode::Phone && !c->early.empty() && onPhone_) {
            web::Bytes early;
            early.swap(c->early);
            onPhone_(early.data(), early.size());
        }
    }
    Sweep(GetTickCount64());
}

void WebServer::Accept() {
    for (int i = 0; i < 8; ++i) {
        sockaddr_in from = {};
        int len = sizeof(from);
        SOCKET s = accept(SOCKET(listen_), reinterpret_cast<sockaddr*>(&from), &len);
        if (s == INVALID_SOCKET) return;
        if (conns_.size() >= kMaxConns) {
            closesocket(s);
            continue;
        }
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        int rcv = 1024 * 1024; // A 4K key frame can be several hundred KB.
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
        auto c = std::make_unique<Conn>();
        c->s = s;
        c->tls = std::make_unique<tls::Session>(cred_.get());
        c->peer = IpText(from.sin_addr.s_addr);
        sockaddr_in local = {};
        int localLen = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLen) == 0) c->localIp = local.sin_addr.s_addr;
        c->opened = c->lastIn = GetTickCount64();
        conns_.push_back(std::move(c));
    }
}

void WebServer::ReadFrom(Conn& c) {
    uint8_t buf[64 * 1024];
    for (int i = 0; i < 32 && !c.dead; ++i) {
        const int n = recv(c.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n == 0) { Close(c, "closed by the page"); return; }
        if (n < 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) return;
            Close(c, "connection lost");
            return;
        }
        c.lastIn = GetTickCount64();
        web::Bytes toSend, plain;
        const bool ok = c.tls->Receive(buf, size_t(n), &toSend, &plain);
        if (!toSend.empty()) SendRaw(c, toSend.data(), toSend.size());
        if (!ok) {
            if (c.tls->Closed()) Close(c, "the page said goodbye");
            else if (!c.tls->Ready()) Close(c, nullptr); // Typically Safari before the certificate is accepted.
            else Close(c, c.tls->Error());
            return;
        }
        if (plain.empty()) continue;
        if (c.mode == Conn::Mode::Http) {
            c.in.insert(c.in.end(), plain.begin(), plain.end());
            HandleHttp(c);
        } else {
            HandleWs(c, plain.data(), plain.size());
        }
    }
}

void WebServer::HandleHttp(Conn& c) {
    while (!c.dead && c.mode == Conn::Mode::Http && !c.in.empty()) {
        web::HttpRequest r;
        size_t head = 0;
        const web::HttpParse p = web::ParseHttpRequest(c.in.data(), c.in.size(), &r, &head);
        if (p == web::HttpParse::Incomplete) return;
        if (p == web::HttpParse::Bad) {
            SendResponse(c, 400, "Bad Request", "text/plain", "bad request\n", "Connection: close\r\n");
            c.closeAfterSend = true;
            return;
        }
        if (c.in.size() < head + r.contentLength) return; // The body hasn't all arrived.
        const std::string body(c.in.begin() + head, c.in.begin() + head + r.contentLength);
        c.in.erase(c.in.begin(), c.in.begin() + head + r.contentLength);
        HandleRequest(c, r, body);
        if (_stricmp(r.Header("connection").c_str(), "close") == 0) c.closeAfterSend = true;
    }
    // After an upgrade, anything left over already belongs to the WebSocket.
    if (!c.dead && c.mode != Conn::Mode::Http && !c.in.empty()) {
        web::Bytes rest;
        rest.swap(c.in);
        HandleWs(c, rest.data(), rest.size());
    }
}

void WebServer::HandleRequest(Conn& c, const web::HttpRequest& r, const std::string& body) {
    if (r.path == "/ws") { HandleUpgrade(c, r); return; }
    if (r.method == "POST" && r.path == "/pair") { HandlePair(c, r, body); return; }
    if (r.method == "POST" && r.path == "/check") {
        const web::WebDevice* d = book_.FindDevice(web::QueryParam(body, "d"));
        if (!d) {
            SendResponse(c, 403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"unknown device\"}");
            return;
        }
        // Busy unless this device's own (older) connection is the one in the way.
        bool other = false, own = false;
        for (auto& o : conns_) {
            if (o->dead || o->mode == Conn::Mode::Http) continue;
            if (o->deviceId != d->id) other = true;
            else own |= o->mode == Conn::Mode::Phone;
        }
        const bool busy = other || (busy_ && !own && d->id != busyExcept_);
        SendResponse(c, 200, "OK", "application/json",
                     std::string("{\"ok\":true,\"busy\":") + (busy ? "true" : "false") + ",\"name\":" + JsonString(d->name) + "}");
        return;
    }
    if (r.method != "GET" && r.method != "HEAD") {
        SendResponse(c, 405, "Method Not Allowed", "text/plain", "method not allowed\n");
        return;
    }
    for (const PageFile& f : kFiles) {
        if (r.path != f.path) continue;
        const std::string data = Resource(f.id);
        if (data.empty()) break;
        std::string extra;
        if (f.id == 301) {
            // The page only talks to this PC.
            extra = "Content-Security-Policy: default-src 'self'; connect-src 'self' wss:; img-src 'self' data:; "
                    "style-src 'self'; script-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n";
        }
        SendResponse(c, 200, "OK", f.type, r.method == "HEAD" ? std::string() : data, extra);
        return;
    }
    SendResponse(c, 404, "Not Found", "text/plain", "not found\n");
}

void WebServer::HandlePair(Conn& c, const web::HttpRequest&, const std::string& body) {
    const uint64_t now = GetTickCount64();
    if (!book_.UsePairingCode(web::QueryParam(body, "t"), now)) {
        Log("web: refused to pair %s: the code is wrong, used or expired", c.peer.c_str());
        SendResponse(c, 403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"code\"}");
        return;
    }
    web::WebDevice d;
    const std::string token =
        book_.AddDevice(web::QueryParam(body, "name"), wifi::Random(32), wifi::Random(8), uint64_t(_time64(nullptr)), &d);
    SaveDevice(d);
    Log("web: paired %s at %s", d.name.c_str(), c.peer.c_str());
    if (pairing_) MakeCode(); // Ready for the next device.
    ++generation_;
    SendResponse(c, 200, "OK", "application/json",
                 "{\"ok\":true,\"device\":" + JsonString(token) + ",\"name\":" + JsonString(d.name) + "}");
}

void WebServer::HandleUpgrade(Conn& c, const web::HttpRequest& r) {
    if (!web::IsWebSocketUpgrade(r)) {
        SendResponse(c, 400, "Bad Request", "text/plain", "expected a WebSocket upgrade\n");
        return;
    }
    // Only this PC's own page may open the link.
    const std::string origin = r.Header("origin");
    if (!origin.empty() && origin != "https://" + r.Header("host")) {
        Log("web: refused a camera link from another site (%s)", c.peer.c_str());
        SendResponse(c, 403, "Forbidden", "text/plain", "wrong origin\n");
        return;
    }
    const web::WebDevice* d = book_.FindDevice(web::QueryParam(r.query, "d"));
    if (!d) {
        Log("web: refused a camera link from %s: no valid device token", c.peer.c_str());
        SendResponse(c, 403, "Forbidden", "text/plain", "unknown device\n");
        return;
    }
    // A newer connection from the same device replaces the old one (Safari reloaded, Wi-Fi changed...).
    // If the old one was running the session, that session ends with it and this one takes over.
    bool other = false, replacedSession = false;
    for (auto& o : conns_) {
        if (o.get() == &c || o->dead || o->mode == Conn::Mode::Http) continue;
        if (o->deviceId != d->id) { other = true; continue; }
        replacedSession |= o->mode == Conn::Mode::Phone;
        const web::Bytes bye = web::CloseFrame(kCloseReplaced);
        web::Bytes enc;
        if (o->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*o, enc.data(), enc.size());
        Flush(*o);
        Close(*o, "replaced by a newer connection");
    }
    if (other || (busy_ && !replacedSession && d->id != busyExcept_)) {
        SendResponse(c, 503, "Service Unavailable", "text/plain", "busy with another phone\n", "Retry-After: 5\r\n");
        return;
    }
    const std::string accept = web::AcceptKey(r.Header("sec-websocket-key"));
    const std::string head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    web::Bytes enc;
    if (!c.tls->Encrypt(reinterpret_cast<const uint8_t*>(head.data()), head.size(), &enc)) {
        Close(c, "TLS failed");
        return;
    }
    SendRaw(c, enc.data(), enc.size());
    c.mode = Conn::Mode::PhonePending;
    c.deviceId = d->id;
    c.deviceName = d->name;
    c.lastIn = GetTickCount64();
    ++generation_;
    Log("web: %s opened the camera link from %s", d->name.c_str(), c.peer.c_str());
}

void WebServer::HandleWs(Conn& c, const uint8_t* data, size_t size) {
    const bool ok = c.ws.Feed(data, size, [&](uint8_t op, const uint8_t* p, size_t n) {
        if (c.dead) return;
        switch (op) {
        case web::kBinary:
            if (c.mode == Conn::Mode::Phone && onPhone_) onPhone_(p, n);
            else if (c.early.size() + n <= kMaxEarly) c.early.insert(c.early.end(), p, p + n);
            break;
        case web::kPing: {
            const web::Bytes pong = web::Frame(web::kPong, p, n);
            web::Bytes enc;
            if (c.tls->Encrypt(pong.data(), pong.size(), &enc)) SendRaw(c, enc.data(), enc.size());
            break;
        }
        case web::kClose: {
            if (!c.closeSent) {
                const web::Bytes bye = web::CloseFrame(1000);
                web::Bytes enc;
                if (c.tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(c, enc.data(), enc.size());
                c.closeSent = true;
            }
            c.closeAfterSend = true;
            break;
        }
        default: break; // Text and pong: nothing to do (lastIn already moved).
        }
    });
    if (!ok && !c.dead) {
        Log("web: %s broke the WebSocket rules (%s)", c.deviceName.c_str(), c.ws.Error());
        const web::Bytes bye = web::CloseFrame(c.ws.TooBig() ? 1009 : 1002);
        web::Bytes enc;
        if (c.tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(c, enc.data(), enc.size());
        Flush(c);
        Close(c, nullptr);
    }
}

void WebServer::SendRaw(Conn& c, const uint8_t* data, size_t size) {
    if (c.dead) return;
    c.out.insert(c.out.end(), data, data + size);
    Flush(c);
}

void WebServer::SendResponse(Conn& c, int code, const char* reason, const std::string& type, const std::string& body,
                             const std::string& extraHeaders) {
    char status[64];
    sprintf_s(status, "HTTP/1.1 %d %s\r\n", code, reason);
    std::string head = status;
    head += "Content-Type: " + type + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    head += extraHeaders;
    head += "\r\n";
    const std::string all = head + body;
    web::Bytes enc;
    if (!c.tls->Encrypt(reinterpret_cast<const uint8_t*>(all.data()), all.size(), &enc)) {
        Close(c, "TLS failed");
        return;
    }
    SendRaw(c, enc.data(), enc.size());
}

void WebServer::Flush(Conn& c) {
    while (!c.dead && c.outPos < c.out.size()) {
        const int n = send(c.s, reinterpret_cast<const char*>(c.out.data() + c.outPos), int(c.out.size() - c.outPos), 0);
        if (n > 0) { c.outPos += size_t(n); continue; }
        if (WSAGetLastError() != WSAEWOULDBLOCK) Close(c, "send failed");
        break;
    }
    if (c.dead) return;
    if (c.outPos == c.out.size()) {
        c.out.clear();
        c.outPos = 0;
        if (c.closeAfterSend) Close(c, nullptr);
    } else if (c.out.size() - c.outPos > kMaxPending) {
        Close(c, "it stopped reading");
    } else if (c.outPos > 64 * 1024) {
        c.out.erase(c.out.begin(), c.out.begin() + c.outPos);
        c.outPos = 0;
    }
}

void WebServer::Close(Conn& c, const char* why) {
    if (c.dead) return;
    if (c.mode != Conn::Mode::Http) {
        Log("web: %s's camera link closed%s%s", c.deviceName.c_str(), why ? ": " : "", why ? why : "");
        ++generation_;
    }
    shutdown(c.s, SD_BOTH);
    closesocket(c.s);
    c.dead = true;
}

void WebServer::Sweep(uint64_t now) {
    const bool ping = now - lastPing_ >= kPingEveryMs;
    if (ping) lastPing_ = now;
    for (auto& cp : conns_) {
        Conn& c = *cp;
        if (c.dead) continue;
        if (c.mode == Conn::Mode::Http) {
            if (!c.tls->Ready() ? now - c.opened > kHandshakeMs : now - c.lastIn > kHttpIdleMs) Close(c, nullptr);
            continue;
        }
        if (now - c.lastIn > kPhoneIdleMs) {
            Close(c, "no answer for 15 s");
            continue;
        }
        if (ping) {
            const web::Bytes f = web::Frame(web::kPing, nullptr, 0);
            web::Bytes enc;
            if (c.tls->Encrypt(f.data(), f.size(), &enc)) SendRaw(c, enc.data(), enc.size());
        }
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](const std::unique_ptr<Conn>& c) { return c->dead; }),
                 conns_.end());
    if (pairing_ && !book_.HasPairingCode(now)) {
        MakeCode(); // Ran out (or was used): the QR code shows a fresh one.
        ++generation_;
    }
}

// --- The phone ---------------------------------------------------------------------------------------

void WebServer::SetBusy(bool busy, const std::string& except) {
    busy_ = busy;
    busyExcept_ = busy ? except : std::string();
    if (!busy) return;
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::PhonePending || (!except.empty() && c->deviceId == except)) continue;
        const web::Bytes bye = web::CloseFrame(kCloseBusy);
        web::Bytes enc;
        if (c->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*c, enc.data(), enc.size());
        Flush(*c);
        Close(*c, "another phone is connected");
    }
}

bool WebServer::PhonePending() const {
    for (auto& c : conns_) if (!c->dead && c->mode == Conn::Mode::PhonePending) return true;
    return false;
}

bool WebServer::TakePhone(std::string* id, std::string* name, std::string* link) {
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::PhonePending) continue;
        c->mode = Conn::Mode::Phone;
        *id = c->deviceId;
        *name = c->deviceName;
        *link = LinkOf(c->localIp);
        ++generation_;
        return true;
    }
    return false;
}

bool WebServer::PhoneConnected() const {
    for (auto& c : conns_) if (!c->dead && c->mode == Conn::Mode::Phone) return true;
    return false;
}

bool WebServer::SendToPhone(const uint8_t* data, size_t size) {
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::Phone) continue;
        const web::Bytes f = web::Frame(web::kBinary, data, size);
        web::Bytes enc;
        if (!c->tls->Encrypt(f.data(), f.size(), &enc)) return false;
        SendRaw(*c, enc.data(), enc.size());
        return !c->dead;
    }
    return false;
}

void WebServer::EndPhone() {
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::Phone) continue;
        const web::Bytes bye = web::CloseFrame(1000);
        web::Bytes enc;
        if (c->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*c, enc.data(), enc.size());
        Flush(*c);
        Close(*c, nullptr);
    }
}

std::string WebServer::LinkOf(uint32_t localIp) const {
    for (const web::NetAddress& a : addresses_) {
        if (a.ipv4 != localIp) continue;
        switch (a.kind) {
        case web::NetKind::WiFi: return "Wi-Fi";
        case web::NetKind::Ethernet: return "network";
        case web::NetKind::IPhoneUsb: return "USB hotspot";
        default: return "network";
        }
    }
    return "network";
}

// --- Pairing ---------------------------------------------------------------------------------------------

void WebServer::RefreshAddresses() {
    std::vector<web::NetAddress> list;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == NO_ERROR) {
        for (auto* a = adapters; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                web::NetAddress n;
                n.ipv4 = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
                n.text = IpText(n.ipv4);
                n.adapter = a->FriendlyName ? a->FriendlyName : L"";
                n.kind = web::ClassifyAdapter(a->IfType, a->Description ? a->Description : L"", ntohl(n.ipv4));
                list.push_back(std::move(n));
            }
        }
    }
    web::SortAddresses(&list);
    addresses_ = std::move(list);
}

void WebServer::MakeCode() { book_.NewPairingCode(wifi::Random(16), GetTickCount64()); }

void WebServer::StartPairing(uint32_t ipv4) {
    RefreshAddresses();
    pairingIp_ = 0;
    for (const web::NetAddress& a : addresses_) if (a.ipv4 == ipv4) pairingIp_ = ipv4;
    if (!pairingIp_ && !addresses_.empty()) pairingIp_ = addresses_[0].ipv4;
    pairing_ = true;
    MakeCode();
    ++generation_;
    if (!pairingIp_) Log("web: pairing asked for, but this PC has no network address a phone could reach");
}

void WebServer::StopPairing() {
    if (!pairing_) return;
    pairing_ = false;
    book_.ClearPairingCode();
    ++generation_;
}

std::string WebServer::PairingUrl() const {
    if (!pairing_ || !pairingIp_ || listen_ == INVALID_SOCKET || book_.PairingCode().empty()) return "";
    return "https://" + IpText(pairingIp_) + ":" + std::to_string(web::kPort) + "/?t=" + book_.PairingCode();
}

// --- Remembered devices: HKCU\Software\MyCam\WebDevices\<id> = TokenHash, Name, Added ----------------------

std::vector<web::WebDevice> WebServer::Devices() const { return book_.Devices(); }

void WebServer::LoadDevices() {
    std::vector<web::WebDevice> list;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kDevicesKey, 0, KEY_READ, &root) == ERROR_SUCCESS) {
        wchar_t sub[64];
        for (DWORD i = 0;; ++i) {
            DWORD len = DWORD(std::size(sub));
            if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            web::WebDevice d;
            d.id = Utf8(sub);
            d.tokenHash.resize(32);
            DWORD size = 32;
            if (RegGetValueW(root, sub, L"TokenHash", RRF_RT_REG_BINARY, nullptr, d.tokenHash.data(), &size) != ERROR_SUCCESS ||
                size != 32)
                continue;
            wchar_t name[128] = {};
            size = sizeof(name);
            if (RegGetValueW(root, sub, L"Name", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) d.name = Utf8(name);
            ULONGLONG added = 0;
            size = sizeof(added);
            if (RegGetValueW(root, sub, L"Added", RRF_RT_REG_QWORD, nullptr, &added, &size) == ERROR_SUCCESS) d.addedUnix = added;
            list.push_back(std::move(d));
        }
        RegCloseKey(root);
    }
    std::sort(list.begin(), list.end(), [](const web::WebDevice& a, const web::WebDevice& b) { return a.addedUnix < b.addedUnix; });
    book_.SetDevices(std::move(list));
}

void WebServer::SaveDevice(const web::WebDevice& d) {
    const std::wstring sub = std::wstring(kDevicesKey) + L"\\" + Wide(d.id);
    RegSetKeyValueW(HKEY_CURRENT_USER, sub.c_str(), L"TokenHash", REG_BINARY, d.tokenHash.data(), DWORD(d.tokenHash.size()));
    const std::wstring name = Wide(d.name);
    RegSetKeyValueW(HKEY_CURRENT_USER, sub.c_str(), L"Name", REG_SZ, name.c_str(), DWORD((name.size() + 1) * sizeof(wchar_t)));
    const ULONGLONG added = d.addedUnix;
    RegSetKeyValueW(HKEY_CURRENT_USER, sub.c_str(), L"Added", REG_QWORD, &added, sizeof(added));
}

void WebServer::Forget(const std::string& id) {
    if (!book_.Forget(id)) return;
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(kDevicesKey) + L"\\" + Wide(id)).c_str());
    for (auto& c : conns_) {
        if (c->dead || c->deviceId != id) continue;
        const web::Bytes bye = web::CloseFrame(kCloseForgotten);
        web::Bytes enc;
        if (c->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*c, enc.data(), enc.size());
        Flush(*c);
        Close(*c, "forgotten on the PC");
    }
    ++generation_;
    Log("web: forgot a device");
}

} // namespace mycam
