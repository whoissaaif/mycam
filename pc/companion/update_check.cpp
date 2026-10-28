#include "update_check.h"

#include <thread>

#include "log.h"
#include "platform.h"

namespace mycam {

namespace {
// Store names (platform.h): the registry under HKCU\Software\MyCam, ~/.config/mycam/store on Linux.
constexpr char kEnabledName[] = "CheckForUpdates";
constexpr char kLatestName[] = "UpdateLatest";
constexpr char kCheckedName[] = "UpdateCheckedAt";
constexpr uint64_t kCheckEverySec = 24 * 60 * 60;
} // namespace

bool UpdateCheckEnabled() {
    uint64_t on = 1; // On unless the user turned it off.
    StoreGetU64("", kEnabledName, &on);
    return on != 0;
}

void SetUpdateCheckEnabled(bool on) {
    StoreSetU64("", kEnabledName, on ? 1 : 0);
    if (!on) StoreSetString("", kLatestName, ""); // Don't keep offering a version we may no longer check.
}

std::string UpdateAvailable(const std::string& currentVersion) {
    if (!UpdateCheckEnabled()) return "";
    std::string latest;
    if (!StoreGetString("", kLatestName, &latest) || latest.empty()) return "";
    return IsNewerVersion(latest, currentVersion) ? latest : "";
}

void StartUpdateCheck(const std::string& currentVersion) {
    if (!UpdateCheckEnabled()) return;
    uint64_t checkedAt = 0;
    StoreGetU64("", kCheckedName, &checkedAt);
    const uint64_t now = UnixTime();
    // now < checkedAt: the clock went backwards, so check again rather than wait for it to catch up.
    if (checkedAt && now >= checkedAt && now - checkedAt < kCheckEverySec) return;
    // Detached: one short request (the platform files set their own timeouts) that must not hold up the UI
    // or the session loop. A failed check writes nothing, so the next start tries again.
    std::thread([currentVersion] {
        std::string body;
        if (!HttpsGet(kReleaseApiHost, kReleaseApiPath, &body)) return;
        const std::string tag = LatestTagFromJson(body);
        if (tag.empty()) return;
        StoreSetU64("", kCheckedName, UnixTime());
        StoreSetString("", kLatestName, tag);
        if (IsNewerVersion(tag, currentVersion)) Log("update check: %s is available", tag.c_str());
    }).detach();
}

} // namespace mycam
