#include "update_version.h"

#include <ctype.h>

namespace mycam {

namespace {

// "1.6.0" -> {1, 6, 0}; *suffix is set for anything after the patch number ("-beta4", "+build7").
bool ParseVersion(const std::string& v, int out[3], bool* suffix) {
    out[0] = out[1] = out[2] = 0;
    *suffix = false;
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        if (i >= v.size() || !isdigit(static_cast<unsigned char>(v[i]))) return false;
        int n = 0;
        while (i < v.size() && isdigit(static_cast<unsigned char>(v[i]))) {
            if (n > 99999) return false; // Not a version of ours, and the next digit would overflow.
            n = n * 10 + (v[i++] - '0');
        }
        out[part] = n;
        if (part < 2) {
            if (i >= v.size() || v[i] != '.') return false;
            ++i;
        }
    }
    *suffix = i < v.size();
    return true;
}

} // namespace

std::string LatestTagFromJson(const std::string& json) {
    // shortcut: finds the first "tag_name" rather than parsing the JSON, which is enough for the one object
    // the releases/latest endpoint returns. Parse it properly if we ever read a list of releases.
    const size_t key = json.find("\"tag_name\"");
    if (key == std::string::npos) return "";
    const size_t colon = json.find(':', key);
    if (colon == std::string::npos) return "";
    const size_t open = json.find('"', colon);
    if (open == std::string::npos) return "";
    const size_t close = json.find('"', open + 1);
    if (close == std::string::npos) return "";
    std::string tag = json.substr(open + 1, close - open - 1);
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    return tag;
}

bool IsNewerVersion(const std::string& latest, const std::string& current) {
    int a[3], b[3];
    bool suffixA = false, suffixB = false;
    if (!ParseVersion(latest, a, &suffixA) || !ParseVersion(current, b, &suffixB)) return false;
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    return suffixB && !suffixA; // Same numbers: only the stable release beats a pre-release of it.
}

} // namespace mycam
