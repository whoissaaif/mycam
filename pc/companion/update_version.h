#pragma once
// Reading GitHub's "latest release" reply and comparing release numbers (issue #79). Kept apart from
// update_check.h because this part touches nothing but the strings, so pc/tests covers it on every platform.

#include <string>

namespace mycam {

// The "tag_name" of a GitHub release reply, without a leading "v" ("" if the reply hasn't got one).
std::string LatestTagFromJson(const std::string& json);

// True if release `latest` comes after `current` ("1.7.0" after "1.6.1"). Both are MAJOR.MINOR.PATCH as
// CLAUDE.md requires, optionally with a pre-release suffix, which loses to the same numbers without one
// ("1.6.0" is newer than "1.6.0-beta4"). Anything else answers false: never offer an update we can't read.
bool IsNewerVersion(const std::string& latest, const std::string& current);

} // namespace mycam
