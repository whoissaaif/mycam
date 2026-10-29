// mycam-tray: the Linux tray icon (IMPROVEMENTS.md §13, phase 4a). A separate program in the desktop session;
// the camera itself stays in the headless mycam-companion service. It watches the service over the control
// socket (control_posix.h) and shows the Windows tray's icons and menu (main.cpp ShowMenu): status, Pause,
// camera, Quality, Mirror, Fill, Connect an iPhone, Reconnect, Start at login, Quit; and the settings window
// (Open MyCam, or `mycam-tray --window`).
//
// The icon goes through AppIndicator (StatusNotifierItem), which Ubuntu's dock shows out of the box. The
// "Connect an iPhone" window is plain GTK for now; it gets the XP look with the shared drawing kit (phase 4b).

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <libayatana-appindicator/app-indicator.h>

#include <algorithm>
#include <string>
#include <vector>

#include "control_proto.h"
#include "platform.h"
#include "status_text.h"
#include "update_check.h" // Only for the releases URL: the daemon does the checking.
#include "../third_party/qrcodegen/qrcodegen.hpp"

namespace mycam {
namespace {

std::string RuntimeDir() {
    const char* dir = getenv("XDG_RUNTIME_DIR");
    return dir && *dir ? dir : "";
}

int Connect() {
    const std::string path = RuntimeDir() + "/mycam.sock";
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (RuntimeDir().empty() || path.size() >= sizeof(addr.sun_path)) return -1;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

bool Send(int s, const std::string& line) {
    const std::string text = line + "\n";
    return s >= 0 && send(s, text.data(), text.size(), MSG_NOSIGNAL) == ssize_t(text.size());
}

// The tray icons are the Windows ones (companion.rc ids 101-105, design/tools/make_icons.ps1): each .ico holds
// PNG frames, and the 32 px one is written out for the dock, which wants a file.
std::string IconPath(int id) {
    const std::string path = RuntimeDir() + "/mycam-tray/icon-" + std::to_string(id) + ".png";
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return path;
    const std::string ico = ResourceData(id);
    if (ico.size() < 6) return "";
    const int count = uint8_t(ico[4]) | uint8_t(ico[5]) << 8;
    for (int i = 0; i < count && 6 + 16 * (i + 1) <= int(ico.size()); ++i) {
        const uint8_t* e = reinterpret_cast<const uint8_t*>(ico.data()) + 6 + 16 * i;
        const uint32_t size = e[8] | e[9] << 8 | e[10] << 16 | uint32_t(e[11]) << 24;
        const uint32_t offset = e[12] | e[13] << 8 | e[14] << 16 | uint32_t(e[15]) << 24;
        if (e[0] != 32 || offset > ico.size() || size > ico.size() - offset) continue;
        g_mkdir_with_parents((RuntimeDir() + "/mycam-tray").c_str(), 0700);
        if (g_file_set_contents(path.c_str(), ico.data() + offset, size, nullptr)) return path;
    }
    return "";
}

// systemctl --user, without a shell. Returns the exit code (-1: couldn't run it).
int Systemctl(const char* verb) {
    const char* argv[] = {"systemctl", "--user", verb, "mycam.service", nullptr};
    int status = -1;
    if (!g_spawn_sync(nullptr, const_cast<char**>(argv), nullptr,
                      GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                      nullptr, nullptr, nullptr, nullptr, &status, nullptr)) {
        return -1;
    }
    return g_spawn_check_wait_status(status, nullptr) ? 0 : 1;
}

// --- Connect an iPhone ---------------------------------------------------------------------------------
// Its own "pair" connection: closing the window closes it, which stops pairing (as Ctrl+C does for --pair).

struct PairWindow {
    GtkWidget* window = nullptr;
    GtkWidget* code = nullptr;
    GtkWidget* note = nullptr;
    int socket = -1;
    guint watch = 0;
    std::string buffer;
    std::string url;
};

PairWindow g_pair;

gboolean DrawQr(GtkWidget* widget, cairo_t* cr, gpointer) {
    const int w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (g_pair.url.empty()) return TRUE;
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(g_pair.url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = qr.getSize() + 8; // A 4-module quiet zone.
    const int cell = std::max(1, std::min(w, h) / modules);
    const int x0 = (w - cell * modules) / 2 + 4 * cell, y0 = (h - cell * modules) / 2 + 4 * cell;
    cairo_set_source_rgb(cr, 0, 0, 0);
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y)) cairo_rectangle(cr, x0 + x * cell, y0 + y * cell, cell, cell);
    cairo_fill(cr);
    return TRUE;
}

void ClosePairing() {
    if (g_pair.watch) g_source_remove(g_pair.watch);
    if (g_pair.socket >= 0) close(g_pair.socket);
    g_pair.watch = 0;
    g_pair.socket = -1;
    g_pair.buffer.clear();
    g_pair.url.clear();
}

gboolean OnPairData(gint fd, GIOCondition, gpointer) {
    char chunk[512];
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
        gtk_label_set_text(GTK_LABEL(g_pair.note), "MyCam stopped answering. Close this window and try again.");
        g_pair.watch = 0;
        ClosePairing();
        return G_SOURCE_REMOVE;
    }
    g_pair.buffer.append(chunk, size_t(n));
    for (size_t nl; (nl = g_pair.buffer.find('\n')) != std::string::npos; g_pair.buffer.erase(0, nl + 1)) {
        const std::string line = g_pair.buffer.substr(0, nl);
        if (line.rfind("url ", 0) == 0) {
            g_pair.url = line.substr(4);
            gtk_widget_queue_draw(g_pair.code);
        } else if (line.rfind("paired ", 0) == 0) {
            const std::string text = "Paired with " + line.substr(7) + ". It connects by itself from now on.";
            gtk_label_set_text(GTK_LABEL(g_pair.note), text.c_str());
        }
    }
    return G_SOURCE_CONTINUE;
}

void ShowPairWindow() {
    if (g_pair.window) {
        gtk_window_present(GTK_WINDOW(g_pair.window));
        return;
    }
    g_pair.socket = Connect();
    if (!Send(g_pair.socket, "pair")) {
        ClosePairing();
        return;
    }
    g_pair.watch = g_unix_fd_add(g_pair.socket, G_IO_IN, OnPairData, nullptr);
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Connect an iPhone - MyCam");
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(window), 16);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    g_pair.code = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_pair.code, 280, 280);
    g_signal_connect(g_pair.code, "draw", G_CALLBACK(DrawQr), nullptr);
    GtkWidget* steps = gtk_label_new(
        "1.  Open the iPhone's Camera app and point it at this code. Tap the link that appears.\n"
        "2.  The first time, Safari says the connection isn’t private. Tap Show Details, then “visit this "
        "website”.\n"
        "3.  Tap Start and allow the camera.\n\n"
        "The iPhone and this PC must be on the same Wi-Fi. Then choose “MyCam” as the camera in any app.");
    gtk_label_set_line_wrap(GTK_LABEL(steps), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(steps), 52);
    gtk_label_set_xalign(GTK_LABEL(steps), 0);
    g_pair.note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_pair.note), 0);
    gtk_box_pack_start(GTK_BOX(box), g_pair.code, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), steps, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), g_pair.note, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
        g_pair.window = nullptr;
        ClosePairing();
    }), nullptr);
    g_pair.window = window;
    gtk_widget_show_all(window);
}

// --- The tray --------------------------------------------------------------------------------------------

struct Tray {
    AppIndicator* indicator = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* pause = nullptr;
    GtkWidget* back = nullptr;
    GtkWidget* front = nullptr;
    GtkWidget* quality = nullptr;
    GtkWidget* qualities[3] = {};
    GtkWidget* mirror = nullptr;
    GtkWidget* fill = nullptr;
    GtkWidget* iphone = nullptr;
    GtkWidget* reconnect = nullptr;
    GtkWidget* wifi = nullptr;
    GtkWidget* autoUsb = nullptr; // "Use plugged-in phones as a webcam automatically" (#77).
    GtkWidget* autostart = nullptr;
    GtkWidget* updates = nullptr;  // "Tell me when a new version is out" (#79).
    GtkWidget* update = nullptr;   // "Version X is available"; hidden while there is nothing newer.
    GtkWidget* start = nullptr;
    GtkWidget* pairing = nullptr; // The "allow this phone?" dialog while a code pairing waits (pairing_dialog.cpp).
    int socket = -1;
    guint watch = 0;
    std::string buffer;
    TrayStatus last;
    bool updating = false; // Setting check marks from a status, not the user clicking them.
};

Tray g_tray;

void SetIcon(int id, const std::string& tooltip) {
    const std::string path = IconPath(id);
    if (!path.empty()) app_indicator_set_icon_full(g_tray.indicator, path.c_str(), tooltip.c_str());
    app_indicator_set_title(g_tray.indicator, tooltip.c_str());
}

// A Wi-Fi phone pairing by code waits for this PC too (PROTOCOL.md "The PC asks too"). Open while the
// daemon says so; closing it without an answer lets the pairing time out, as on Windows.
void ShowPairing(const TrayStatus& s, bool running) {
    if (!(running && s.pairing)) {
        if (g_tray.pairing) gtk_widget_destroy(g_tray.pairing);
        return;
    }
    if (g_tray.pairing) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(g_tray.pairing), "%s", s.detail.c_str());
        return;
    }
    g_tray.pairing = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                            s.headline.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(g_tray.pairing), "%s", s.detail.c_str());
    gtk_window_set_title(GTK_WINDOW(g_tray.pairing), "MyCam");
    gtk_window_set_keep_above(GTK_WINDOW(g_tray.pairing), TRUE);
    gtk_dialog_add_buttons(GTK_DIALOG(g_tray.pairing), "_Don\u2019t allow", GTK_RESPONSE_REJECT, "_Allow",
                           GTK_RESPONSE_ACCEPT, nullptr);
    g_signal_connect(g_tray.pairing, "response", G_CALLBACK(+[](GtkDialog* d, gint response, gpointer) {
        if (response == GTK_RESPONSE_ACCEPT) Send(g_tray.socket, "allow");
        else if (response == GTK_RESPONSE_REJECT) Send(g_tray.socket, "deny");
        gtk_widget_destroy(GTK_WIDGET(d));
    }), nullptr);
    g_signal_connect(g_tray.pairing, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { g_tray.pairing = nullptr; }),
                     nullptr);
    gtk_widget_show_all(g_tray.pairing);
    gtk_window_present(GTK_WINDOW(g_tray.pairing));
}

void UpdateWindow(const TrayStatus& s, bool running); // The settings window, below.

void ShowStatus(const TrayStatus& s, bool running) {
    g_tray.updating = true;
    g_tray.last = s;
    const std::string headline = running ? s.headline : "MyCam isn't running";
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.status), ("MyCam: " + headline).c_str());
    SetIcon(running ? s.icon : kIconDisconnected, running ? "MyCam: " + headline + (s.detail.empty() ? "" : "\n" + s.detail)
                                                          : "MyCam: " + headline);
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.pause), s.paused ? "Resume camera" : "Pause camera");
    gtk_widget_set_sensitive(g_tray.pause, running && s.canPause);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s.facing ? g_tray.front : g_tray.back), TRUE);
    for (GtkWidget* w : {g_tray.back, g_tray.front, g_tray.mirror, g_tray.fill, g_tray.iphone, g_tray.reconnect,
                         g_tray.wifi, g_tray.autoUsb, g_tray.updates})
        gtk_widget_set_sensitive(w, running);
    gtk_widget_set_sensitive(g_tray.quality, running && s.linked);
    for (int q = 0; q < 3; ++q) {
        gtk_widget_set_sensitive(g_tray.qualities[q], s.qualityOk[q]);
        if (q == s.quality) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.qualities[q]), TRUE);
    }
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.mirror), s.mirror);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.fill), s.fill);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.wifi), s.wifi);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.autoUsb), s.autoUsb);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.updates), s.updates);
    if (!s.update.empty()) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.update), ("Version " + s.update + " is available").c_str());
    }
    gtk_widget_set_visible(g_tray.update, running && !s.update.empty());
    gtk_widget_set_visible(g_tray.start, !running);
    UpdateWindow(s, running);
    g_tray.updating = false;
    ShowPairing(s, running);
}

void Disconnected();

gboolean OnData(gint fd, GIOCondition, gpointer) {
    char chunk[1024];
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0 || g_tray.buffer.size() > 16384) {
        g_tray.watch = 0;
        Disconnected();
        return G_SOURCE_REMOVE;
    }
    g_tray.buffer.append(chunk, size_t(n));
    for (size_t nl; (nl = g_tray.buffer.find('\n')) != std::string::npos; g_tray.buffer.erase(0, nl + 1)) {
        TrayStatus s;
        if (ParseTrayStatus(g_tray.buffer.substr(0, nl), &s)) ShowStatus(s, true);
    }
    return G_SOURCE_CONTINUE;
}

gboolean TryConnect(gpointer) {
    if (g_tray.socket >= 0) return G_SOURCE_CONTINUE;
    const int s = Connect();
    if (s < 0) return G_SOURCE_CONTINUE;
    if (!Send(s, "watch")) {
        close(s);
        return G_SOURCE_CONTINUE;
    }
    g_tray.socket = s;
    g_tray.watch = g_unix_fd_add(s, G_IO_IN, OnData, nullptr);
    return G_SOURCE_CONTINUE;
}

void Disconnected() {
    if (g_tray.watch) g_source_remove(g_tray.watch);
    if (g_tray.socket >= 0) close(g_tray.socket);
    g_tray.watch = 0;
    g_tray.socket = -1;
    g_tray.buffer.clear();
    ShowStatus(TrayStatus(), false); // Tries again every 2 s (main).
}

void Command(const std::string& line) {
    if (!g_tray.updating) Send(g_tray.socket, line);
}

GtkWidget* Item(GtkWidget* menu, const char* label, void (*onClick)()) {
    GtkWidget* item = gtk_menu_item_new_with_mnemonic(label);
    if (onClick) g_signal_connect_swapped(item, "activate", G_CALLBACK(onClick), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget* Check(GtkWidget* menu, GSList** group, const char* label, void (*onToggle)(GtkCheckMenuItem*)) {
    GtkWidget* item = group ? gtk_radio_menu_item_new_with_mnemonic(*group, label) : gtk_check_menu_item_new_with_mnemonic(label);
    if (group) *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    g_signal_connect(item, "toggled", G_CALLBACK(onToggle), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

void Separator(GtkWidget* menu) { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); }

bool Active(GtkCheckMenuItem* item) { return gtk_check_menu_item_get_active(item); }

void ShowWindow();

GtkWidget* BuildMenu() {
    GtkWidget* menu = gtk_menu_new();
    g_tray.status = Item(menu, "MyCam", nullptr);
    gtk_widget_set_sensitive(g_tray.status, FALSE);
    Item(menu, "_Open MyCam", ShowWindow);
    g_tray.start = Item(menu, "_Start MyCam", [] { Systemctl("start"); });
    Separator(menu);
    g_tray.pause = Item(menu, "_Pause camera", [] { Command(g_tray.last.paused ? "pause 0" : "pause 1"); });
    Separator(menu);
    GSList* facing = nullptr;
    g_tray.back = Check(menu, &facing, "_Back camera", [](GtkCheckMenuItem* i) { if (Active(i)) Command("facing 0"); });
    g_tray.front = Check(menu, &facing, "_Front camera", [](GtkCheckMenuItem* i) { if (Active(i)) Command("facing 1"); });
    g_tray.quality = Item(menu, "_Quality", nullptr);
    GtkWidget* qualityMenu = gtk_menu_new();
    GSList* quality = nullptr;
    g_tray.qualities[0] = Check(qualityMenu, &quality, "720p", [](GtkCheckMenuItem* i) { if (Active(i)) Command("quality 0"); });
    g_tray.qualities[1] = Check(qualityMenu, &quality, "1080p", [](GtkCheckMenuItem* i) { if (Active(i)) Command("quality 1"); });
    g_tray.qualities[2] = Check(qualityMenu, &quality, "4K", [](GtkCheckMenuItem* i) { if (Active(i)) Command("quality 2"); });
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(g_tray.quality), qualityMenu);
    g_tray.mirror = Check(menu, nullptr, "_Mirror image", [](GtkCheckMenuItem* i) { Command(Active(i) ? "mirror 1" : "mirror 0"); });
    g_tray.fill = Check(menu, nullptr, "Fill the frame", [](GtkCheckMenuItem* i) { Command(Active(i) ? "fill 1" : "fill 0"); });
    Separator(menu);
    g_tray.iphone = Item(menu, "Connect an _iPhone…", ShowPairWindow);
    g_tray.reconnect = Item(menu, "Reconnect phone", [] { Command("reconnect"); });
    g_tray.wifi = Check(menu, nullptr, "Find phones on _Wi-Fi", [](GtkCheckMenuItem* i) { Command(Active(i) ? "wifi 1" : "wifi 0"); });
    g_tray.autoUsb = Check(menu, nullptr, "Use plugged-in phones as a _webcam",
                           [](GtkCheckMenuItem* i) { Command(Active(i) ? "autousb 1" : "autousb 0"); });
    g_tray.autostart = Check(menu, nullptr, "Start at login", [](GtkCheckMenuItem* i) {
        if (!g_tray.updating) Systemctl(Active(i) ? "enable" : "disable");
    });
    g_tray.updates = Check(menu, nullptr, "_Tell me when a new version is out",
                           [](GtkCheckMenuItem* i) { Command(Active(i) ? "updates 1" : "updates 0"); });
    // Only there when the daemon's last check found something newer; the label carries the version.
    g_tray.update = Item(menu, "A new version is available", [] {
        g_app_info_launch_default_for_uri(kReleasesPageUrl, nullptr, nullptr);
    });
    gtk_widget_set_no_show_all(g_tray.update, TRUE);
    gtk_widget_hide(g_tray.update);
    Separator(menu);
    Item(menu, "Quit MyCam", [] {
        Systemctl("stop"); // Like Exit on Windows: the camera goes too. "Start at login" brings it back.
        g_application_quit(g_application_get_default());
    });
    gtk_widget_show_all(menu);
    return menu;
}

// --- The settings window -----------------------------------------------------------------------------------
// The Windows window's pages (settings_window.cpp: Home, Devices, Camera, Settings, About) in plain GTK, with
// its v2 palette (xp_draw.cpp) as CSS. It shows the same status line as the menu and sends the same commands.
// ponytail: GTK widgets, not the Direct2D drawing; the shared XP drawing kit (phase 4b) replaces this when it lands.

constexpr char kWindowCss[] = R"(
.mycam, .mycam .page { background: #F5F7FA; color: #1B2330; font-family: Tahoma, "DejaVu Sans", sans-serif; }
.mycam .nav { background: #FFFFFF; border-right: 1px solid #E3E8EF; }
.mycam .nav row { padding: 8px 14px; color: #1B2330; }
.mycam .nav row:selected { background: #1E6FE8; color: #FFFFFF; }
.mycam .card { background: #FFFFFF; border: 1px solid #E3E8EF; border-radius: 8px; padding: 14px; }
.mycam .title { font-size: 18px; font-weight: bold; }
.mycam .headline { font-size: 15px; font-weight: bold; }
.mycam .subtle { color: #5C6675; }
.mycam button { background: #FFFFFF; color: #1B2330; border: 1px solid #D0D7E2; border-radius: 6px; padding: 4px 14px;
                box-shadow: none; text-shadow: none; -gtk-icon-shadow: none; }
.mycam button:hover { border-color: #A8B7CC; }
.mycam button.primary { background: #1E6FE8; color: #FFFFFF; border-color: #1760D0; }
.mycam button.primary:hover { background: #3281F0; }
.mycam button:disabled, .mycam button:disabled label { color: #A3ACBA; }
.mycam switch { background: #D7DEE8; border: 1px solid #B4C0D0; }
.mycam switch:checked { background: #1E6FE8; border-color: #1760D0; }
.mycam switch slider { background: #FFFFFF; border: 1px solid #B4C0D0; }
.mycam radiobutton label { color: #1B2330; }
.mycam radiobutton:disabled label { color: #A3ACBA; }
)";

struct Window {
    GtkWidget* window = nullptr;
    GtkWidget* icon = nullptr;
    GtkWidget* headline = nullptr;
    GtkWidget* detail = nullptr;
    GtkWidget* pause = nullptr;
    GtkWidget* start = nullptr;
    GtkWidget* back = nullptr;
    GtkWidget* front = nullptr;
    GtkWidget* qualities[3] = {};
    GtkWidget* cameraHint = nullptr;
    GtkWidget* mirror = nullptr;
    GtkWidget* fill = nullptr;
    GtkWidget* wifi = nullptr;
    GtkWidget* autoUsb = nullptr;
    GtkWidget* autostart = nullptr;
    GtkWidget* updates = nullptr;
    GtkWidget* update = nullptr;
    std::vector<GtkWidget*> live; // Only usable while the service runs.
};

Window g_win;

GtkWidget* Label(const std::string& text, const char* cls = nullptr) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    if (cls) gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

GtkWidget* Card(GtkWidget* page) {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    return card;
}

GtkWidget* Page(GtkWidget* stack, const char* name, const char* title, const char* subtitle) {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 16);
    gtk_style_context_add_class(gtk_widget_get_style_context(page), "page");
    gtk_box_pack_start(GTK_BOX(page), Label(title, "title"), FALSE, FALSE, 0);
    if (subtitle) gtk_box_pack_start(GTK_BOX(page), Label(subtitle, "subtle"), FALSE, FALSE, 0);
    gtk_stack_add_titled(GTK_STACK(stack), page, name, name);
    return page;
}

GtkWidget* Button(GtkWidget* box, const char* label, void (*onClick)(), bool primary = false) {
    GtkWidget* b = gtk_button_new_with_mnemonic(label);
    if (primary) gtk_style_context_add_class(gtk_widget_get_style_context(b), "primary");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(onClick), nullptr);
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    return b;
}

// A labelled on/off switch: `name` and its sentence on the left, the switch on the right.
GtkWidget* Switch(GtkWidget* card, const char* name, const char* sentence, void (*onToggle)(bool)) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(text), Label(name), FALSE, FALSE, 0);
    if (sentence) gtk_box_pack_start(GTK_BOX(text), Label(sentence, "subtle"), FALSE, FALSE, 0);
    GtkWidget* sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_signal_connect(sw, "notify::active", G_CALLBACK(+[](GtkSwitch* s, GParamSpec*, gpointer f) {
        if (!g_tray.updating) reinterpret_cast<void (*)(bool)>(f)(gtk_switch_get_active(s));
    }), reinterpret_cast<gpointer>(onToggle));
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
    return sw;
}

GtkWidget* Radio(GtkWidget* box, GtkWidget* group, const char* label, const char* command) {
    GtkWidget* r = group ? gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(group), label)
                         : gtk_radio_button_new_with_mnemonic(nullptr, label);
    g_signal_connect(r, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer c) {
        if (gtk_toggle_button_get_active(b)) Command(static_cast<const char*>(c));
    }), const_cast<char*>(command));
    gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    return r;
}

void Live(GtkWidget* w) { g_win.live.push_back(w); }

void BuildHome(GtkWidget* stack) {
    GtkWidget* page = Page(stack, "Home", "Home", nullptr);
    GtkWidget* card = Card(page);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_win.icon = gtk_image_new();
    gtk_widget_set_valign(g_win.icon, GTK_ALIGN_START);
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_win.headline = Label("", "headline");
    g_win.detail = Label("", "subtle");
    gtk_box_pack_start(GTK_BOX(text), g_win.headline, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), g_win.detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), g_win.icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_win.pause = Button(buttons, "_Pause camera", [] { Command(g_tray.last.paused ? "pause 0" : "pause 1"); }, true);
    g_win.start = Button(buttons, "_Start MyCam", [] { Systemctl("start"); }, true);
    gtk_widget_set_no_show_all(g_win.start, TRUE);
    Live(Button(buttons, "Connect an _iPhone…", ShowPairWindow));
    Live(Button(buttons, "_Reconnect phone", [] { Command("reconnect"); }));
    gtk_box_pack_start(GTK_BOX(card), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       Label("Choose “MyCam” as the camera in Zoom, OBS, Discord or your browser.", "subtle"),
                       FALSE, FALSE, 0);
}

void BuildDevices(GtkWidget* stack) {
    GtkWidget* page = Page(stack, "Devices", "Devices", "How phones connect to this PC.");
    GtkWidget* card = Card(page);
    g_win.autoUsb = Switch(card, "Use plugged-in phones as a webcam",
                           "Off: a plugged-in phone stays free for charging and file transfer; Reconnect phone "
                           "switches it on demand.",
                           [](bool on) { Command(on ? "autousb 1" : "autousb 0"); });
    g_win.wifi = Switch(card, "Find phones on Wi-Fi", "Android phones with MyCam on the same Wi-Fi.",
                        [](bool on) { Command(on ? "wifi 1" : "wifi 0"); });
    Live(g_win.autoUsb);
    Live(g_win.wifi);
    GtkWidget* iphone = Card(page);
    gtk_box_pack_start(GTK_BOX(iphone), Label("iPhone", "headline"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(iphone), Label("An iPhone connects over Wi-Fi from Safari: scan a QR code once.", "subtle"),
                       FALSE, FALSE, 0);
    Live(Button(iphone, "Connect an _iPhone…", ShowPairWindow));
}

void BuildCamera(GtkWidget* stack) {
    GtkWidget* page = Page(stack, "Camera", "Camera", nullptr);
    GtkWidget* card = Card(page);
    gtk_box_pack_start(GTK_BOX(card), Label("Camera"), FALSE, FALSE, 0);
    GtkWidget* facing = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    g_win.back = Radio(facing, nullptr, "_Back", "facing 0");
    g_win.front = Radio(facing, g_win.back, "_Front", "facing 1");
    gtk_box_pack_start(GTK_BOX(card), facing, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), Label("Resolution"), FALSE, FALSE, 0);
    GtkWidget* quality = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    g_win.qualities[0] = Radio(quality, nullptr, "720p", "quality 0");
    g_win.qualities[1] = Radio(quality, g_win.qualities[0], "1080p", "quality 1");
    g_win.qualities[2] = Radio(quality, g_win.qualities[0], "4K", "quality 2");
    gtk_box_pack_start(GTK_BOX(card), quality, FALSE, FALSE, 0);
    g_win.cameraHint = Label("Connect a phone to choose.", "subtle");
    gtk_widget_set_no_show_all(g_win.cameraHint, TRUE);
    gtk_box_pack_start(GTK_BOX(card), g_win.cameraHint, FALSE, FALSE, 0);
    Live(g_win.back);
    Live(g_win.front);
    GtkWidget* picture = Card(page);
    g_win.mirror = Switch(picture, "Mirror image", nullptr, [](bool on) { Command(on ? "mirror 1" : "mirror 0"); });
    g_win.fill = Switch(picture, "Fill the frame", "Crop to fill instead of showing black bars.",
                        [](bool on) { Command(on ? "fill 1" : "fill 0"); });
    Live(g_win.mirror);
    Live(g_win.fill);
}

void BuildSettings(GtkWidget* stack) {
    GtkWidget* page = Page(stack, "Settings", "Settings", nullptr);
    GtkWidget* card = Card(page);
    g_win.autostart = Switch(card, "Start at login", nullptr, [](bool on) {
        Systemctl(on ? "enable" : "disable");
        g_tray.updating = true; // Keep the menu's check mark in step.
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.autostart), on);
        g_tray.updating = false;
    });
    g_win.updates = Switch(card, "Tell me when a new version is out", "One check a day with GitHub.",
                           [](bool on) { Command(on ? "updates 1" : "updates 0"); });
    Live(g_win.updates);
    GtkWidget* quit = Card(page);
    gtk_box_pack_start(GTK_BOX(quit), Label("Quit MyCam stops the camera too. It starts again at the next login.", "subtle"),
                       FALSE, FALSE, 0);
    Button(quit, "_Quit MyCam", [] {
        Systemctl("stop");
        g_application_quit(g_application_get_default());
    });
}

void BuildAbout(GtkWidget* stack) {
    GtkWidget* page = Page(stack, "About", "About", nullptr);
    GtkWidget* card = Card(page);
    GtkWidget* logo = gtk_image_new_from_icon_name("mycam", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 96);
    gtk_widget_set_halign(logo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), logo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), Label("MyCam", "headline"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), Label("Version " MYCAM_VERSION, "subtle"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), Label("Use your phone as a webcam. Free and open source (MIT)."), FALSE, FALSE, 0);
    g_win.update = Button(card, "A new version is available", [] {
        g_app_info_launch_default_for_uri(kReleasesPageUrl, nullptr, nullptr);
    }, true);
    gtk_widget_set_no_show_all(g_win.update, TRUE);
    Button(card, "_Releases and source", [] {
        g_app_info_launch_default_for_uri("https://github.com/whoissaaif/mycam", nullptr, nullptr);
    });
}

void UpdateWindow(const TrayStatus& s, bool running) {
    if (!g_win.window) return;
    const std::string path = IconPath(running ? s.icon : kIconDisconnected);
    if (!path.empty()) gtk_image_set_from_file(GTK_IMAGE(g_win.icon), path.c_str());
    gtk_label_set_text(GTK_LABEL(g_win.headline), running ? s.headline.c_str() : "MyCam isn't running");
    gtk_label_set_text(GTK_LABEL(g_win.detail),
                       running ? s.detail.c_str() : "The camera service is stopped. Start it to use your phone.");
    gtk_button_set_label(GTK_BUTTON(g_win.pause), s.paused ? "_Resume camera" : "_Pause camera");
    gtk_widget_set_visible(g_win.pause, running);
    gtk_widget_set_sensitive(g_win.pause, s.canPause);
    gtk_widget_set_visible(g_win.start, !running);
    for (GtkWidget* w : g_win.live) gtk_widget_set_sensitive(w, running);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s.facing ? g_win.front : g_win.back), TRUE);
    for (int q = 0; q < 3; ++q) {
        gtk_widget_set_sensitive(g_win.qualities[q], running && s.linked && s.qualityOk[q]);
        if (q == s.quality) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_win.qualities[q]), TRUE);
    }
    gtk_widget_set_visible(g_win.cameraHint, !(running && s.linked));
    gtk_switch_set_active(GTK_SWITCH(g_win.mirror), s.mirror);
    gtk_switch_set_active(GTK_SWITCH(g_win.fill), s.fill);
    gtk_switch_set_active(GTK_SWITCH(g_win.wifi), s.wifi);
    gtk_switch_set_active(GTK_SWITCH(g_win.autoUsb), s.autoUsb);
    gtk_switch_set_active(GTK_SWITCH(g_win.updates), s.updates);
    gtk_switch_set_active(GTK_SWITCH(g_win.autostart),
                          gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(g_tray.autostart)));
    if (!s.update.empty()) gtk_button_set_label(GTK_BUTTON(g_win.update), ("Get version " + s.update).c_str());
    gtk_widget_set_visible(g_win.update, running && !s.update.empty());
}

void ShowWindow() {
    if (g_win.window) {
        gtk_window_present(GTK_WINDOW(g_win.window));
        return;
    }
    static GtkCssProvider* css = nullptr;
    if (!css) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, kWindowCss, -1, nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        // Luna only, never dark (CLAUDE.md): a dark system theme still paints the parts the CSS leaves alone.
        g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", FALSE, nullptr);
    }
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_application(GTK_WINDOW(window), GTK_APPLICATION(g_application_get_default()));
    gtk_window_set_title(GTK_WINDOW(window), "MyCam");
    gtk_window_set_icon_name(GTK_WINDOW(window), "mycam");
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 500);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "mycam");
    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
    g_win = Window();
    g_win.window = window;
    BuildHome(stack);
    BuildDevices(stack);
    BuildCamera(stack);
    BuildSettings(stack);
    BuildAbout(stack);
    GtkWidget* nav = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(nav), GTK_STACK(stack));
    gtk_widget_set_size_request(nav, 150, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(nav), "nav");
    GtkWidget* columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(columns), nav, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(columns), stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(window), columns);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { g_win = Window(); }), nullptr);
    gtk_widget_show_all(window);
    g_tray.updating = true;
    UpdateWindow(g_tray.last, g_tray.socket >= 0);
    g_tray.updating = false;
    gtk_window_present(GTK_WINDOW(window));
}

} // namespace
} // namespace mycam

int main(int argc, char** argv) {
    using namespace mycam;
    if (!getenv("XDG_RUNTIME_DIR") || !*getenv("XDG_RUNTIME_DIR")) {
        fprintf(stderr, "mycam-tray: no XDG_RUNTIME_DIR; start it from the desktop session\n");
        return 1;
    }
    // One tray per session, through GtkApplication: a second start (autostart plus a menu launch, say) only
    // hands its command line to the running tray. --window (the MyCam entry in the apps) opens the window; the
    // launch's activation token comes along, so the desktop lets the window come to the front.
    GtkApplication* app = gtk_application_new("io.github.whoissaaif.mycam.tray", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "startup", G_CALLBACK(+[](GApplication* a, gpointer) {
        g_application_hold(a); // No window most of the time: the tray keeps it running.
        g_tray.indicator = app_indicator_new("mycam", "camera-web", APP_INDICATOR_CATEGORY_HARDWARE);
        app_indicator_set_status(g_tray.indicator, APP_INDICATOR_STATUS_ACTIVE);
        app_indicator_set_menu(g_tray.indicator, GTK_MENU(BuildMenu()));
        ShowStatus(TrayStatus(), false);
        g_tray.updating = true;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.autostart), Systemctl("is-enabled") == 0);
        g_tray.updating = false;
        TryConnect(nullptr);
        g_timeout_add_seconds(2, TryConnect, nullptr);
    }), nullptr);
    g_signal_connect(app, "command-line", G_CALLBACK(+[](GApplication*, GApplicationCommandLine* cl, gpointer) {
        gint n = 0;
        gchar** args = g_application_command_line_get_arguments(cl, &n);
        if (n > 1 && strcmp(args[1], "--window") == 0) ShowWindow();
        g_strfreev(args);
        return 0;
    }), nullptr);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
