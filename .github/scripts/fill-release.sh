#!/usr/bin/env bash
# Fills the website's @@...@@ placeholders from this repo's latest GitHub release, so mycam.dev always
# offers it (pages.yml runs this on every deploy and whenever a release is published). An expected file
# missing from that release links its release page instead of a download that would 404.
#   @@VERSION@@          1.7.0
#   @@EXE_URL@@ ...      a download link (or the release page)
#   @@EXE_SIZE@@ ...     " · 12.1 MB" (or nothing)
set -euo pipefail
dir=${1:?usage: fill-release.sh <website dir>}
api="repos/${GITHUB_REPOSITORY:?}/releases/latest"
# One line per file: "name|download url|size", then the tag and the release page (gh's own jq: no jq needed).
assets=$(gh api "$api" --jq '.assets[] | "\(.name)|\(.browser_download_url)|\(.size)"')
tag=$(gh api "$api" --jq .tag_name)
version=${tag#v}
page=$(gh api "$api" --jq .html_url)

fill() { # placeholder key, asset file name
    local hit url size
    hit=$(awk -F'|' -v n="$2" '$1 == n { print $2 "|" $3 }' <<<"$assets")
    if [ -n "$hit" ]; then
        url=${hit%|*}
        size=$(awk -v b="${hit#*|}" 'BEGIN { printf " · %.1f MB", b / 1048576 }')
    else
        echo "::warning::$2 isn't in $tag: linking the release page instead"
        url=$page
        size=""
    fi
    sed -i "s|@@${1}_URL@@|${url}|g; s|@@${1}_SIZE@@|${size}|g" "$dir"/*.html
}

fill EXE "MyCam-Setup-${version}.exe"
fill APK "MyCam-${version}.apk"
fill DEB "mycam_${version}_amd64.deb"
sed -i "s|@@VERSION@@|${version}|g" "$dir"/*.html
if grep -n "@@[A-Z_]*@@" "$dir"/*.html; then
    echo "::error::placeholders left in the website"
    exit 1
fi
echo "website filled in for ${tag}"
