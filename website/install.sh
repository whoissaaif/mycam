#!/bin/sh
# MyCam for Linux (beta), installed from the command line:
#
#   wget -qO- https://mycam.dev/install.sh | sh      (Ubuntu has wget out of the box)
#   curl -fsSL https://mycam.dev/install.sh | sh
#
# Downloads the .deb from the latest GitHub release (https://github.com/whoissaaif/mycam/releases/latest),
# installs it with apt (which also brings v4l2loopback-dkms, the camera device), and starts MyCam and its
# tray icon for you. Ubuntu, Debian and their relatives on amd64. Remove it again with: sudo apt remove mycam
set -eu

repo=whoissaaif/mycam
guide="https://github.com/$repo/blob/main/pc/linux/README.md"

say() { printf 'MyCam: %s\n' "$*"; }
fail() {
    printf 'MyCam: %s\n' "$*" >&2
    exit 1
}

[ "$(uname -s)" = Linux ] || fail "this installer is for Linux. Windows and Android: https://mycam.dev/download.html"
command -v apt-get >/dev/null 2>&1 || fail "this installer needs apt (Ubuntu, Debian, Mint, Pop!_OS...). See $guide"
arch=$(dpkg --print-architecture)
[ "$arch" = amd64 ] || fail "there is only an amd64 build so far (this PC is $arch). See $guide"
# curl or wget, whichever is there: a fresh Ubuntu desktop has wget but not curl.
if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL -H "Accept: application/vnd.github+json" "$1"; }
    download() { curl -fL --progress-bar -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -qO- --header="Accept: application/vnd.github+json" "$1"; }
    download() { wget -q --show-progress -O "$2" "$1"; }
else
    fail "this installer needs curl or wget: sudo apt install wget"
fi
if [ "$(id -u)" -eq 0 ]; then
    sudo=""
else
    command -v sudo >/dev/null 2>&1 || fail "this installer needs sudo, or run it as root"
    sudo=sudo
fi

# The .deb of the latest release (no jq needed: its download link is the only one ending in _amd64.deb).
url=$(fetch "https://api.github.com/repos/$repo/releases/latest" |
    grep -o '"browser_download_url": *"[^"]*_amd64\.deb"' | head -n 1 | sed 's/.*"\(https[^"]*\)"$/\1/')
[ -n "$url" ] || fail "couldn't find the Linux package in the latest release: https://github.com/$repo/releases/latest"
deb=${url##*/}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
chmod 755 "$tmp" # apt reads local packages as its own user.
say "downloading $deb"
download "$url" "$tmp/$deb"
chmod 644 "$tmp/$deb"

say "installing (sudo may ask for your password)"
# $sudo is empty when running as root, so it is left unquoted on purpose.
# shellcheck disable=SC2086
$sudo apt-get update -qq || say "couldn't refresh the package lists; installing anyway"
# shellcheck disable=SC2086
$sudo apt-get install -y "$tmp/$deb"

# Start it now rather than at the next login (the package enables both for every login from now on).
if [ "$(id -u)" -ne 0 ] && command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
    systemctl --user daemon-reload || true
    systemctl --user start mycam && say "MyCam is running"
fi
if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v mycam-tray >/dev/null 2>&1 && ! pgrep -x mycam-tray >/dev/null 2>&1; then
    (setsid mycam-tray >/dev/null 2>&1 &) || true
fi

say "installed $deb. Choose \"MyCam\" as the camera in any app."
say "Android: open MyCam on the phone (Wi-Fi: same network as this PC). iPhone: mycam-companion --pair"
say "If it said v4l2loopback is missing, reboot once. Guide: $guide"
