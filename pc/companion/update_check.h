#pragma once
// "Version X is available" on the About pages (issue #79). One HTTPS GET of this project's latest GitHub
// release, at most once a day, on a thread of its own, so nothing is sent beyond the request itself. One
// setting turns it off (CheckForUpdates in the store, on unless somebody turned it off), and the answer is
// remembered, so the UI only ever reads what the last check found and never waits for the network.
//
// The GET is per platform: update_check_win.cpp (WinHTTP) and update_check_openssl.cpp (OpenSSL).

#include <string>

#include "update_version.h"

namespace mycam {

// Where "Version X is available" goes, and what the check asks GitHub for.
constexpr char kReleasesPageUrl[] = "https://github.com/whoissaaif/mycam/releases/latest";
constexpr char kReleaseApiHost[] = "api.github.com";
constexpr char kReleaseApiPath[] = "/repos/whoissaaif/mycam/releases/latest";

bool UpdateCheckEnabled();           // The setting. On unless it was turned off.
void SetUpdateCheckEnabled(bool on); // Turning it off also forgets what the last check found.

// The newer release to offer, or "": nothing newer, never checked, or turned off. Reads the remembered
// answer only, so the UI thread may call it as often as it likes.
std::string UpdateAvailable(const std::string& currentVersion);

// Starts one check in the background if the setting is on and the last one was over a day ago, and returns
// at once. Call it at start-up; the answer is there for the next time the About page is opened.
void StartUpdateCheck(const std::string& currentVersion);

// Per platform: GETs https://host/path. False if the request failed or the reply wasn't a 200.
bool HttpsGet(const char* host, const char* path, std::string* body);

} // namespace mycam
