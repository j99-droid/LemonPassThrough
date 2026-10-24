#!/usr/bin/env bash
# Lemon Pass Through installer.
# Builds and installs the PC server, adds a launcher, and installs the Quest app over adb.
#
#   ./install.sh                     everything: dependencies, server, headset app
#   ./install.sh --headset 192.168.1.50
#                                    install the headset app over Wi-Fi (adb on port 5555)
#   ./install.sh --skip-deps         don't run apt-get (dependencies already installed)
#   ./install.sh --server-only       skip the headset app
#   ./install.sh --headset-only      only install the headset app
set -euo pipefail

REPO="j99-droid/LemonPassThrough"
APK_URL="https://github.com/$REPO/releases/latest/download/LemonPassThrough.apk"
APP_ID="org.meumeu.wivrn.local"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${LEMON_PREFIX:-$HOME/.local/share/lemon-pass-through}"
BUILD="$ROOT/build"
TOOLS="$ROOT/.tools"

CMAKE_VERSION=3.31.6
CMAKE_SHA256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
VULKAN_HEADERS_TAG=vulkan-sdk-1.4.363.0
VULKAN_HEADERS_SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e

# Build dependencies on Ubuntu 24.04 / Pop!_OS 24.04 and newer
APT_PACKAGES=(
	build-essential ninja-build git pkg-config curl
	glslang-tools spirv-tools librsvg2-bin
	libvulkan-dev libopenxr-dev libssl-dev libcurl4-openssl-dev libarchive-dev
	libboost-locale-dev libboost-url-dev libboost-iostreams-dev
	libx264-dev libavcodec-dev libavutil-dev libswscale-dev libva-dev
	libavahi-client-dev libavahi-glib-dev libpipewire-0.3-dev libsystemd-dev libnotify-dev
	libeigen3-dev libudev-dev libwayland-dev libxrandr-dev libxcb-randr0-dev libgl-dev libglx-dev
	nlohmann-json3-dev libcli11-dev libfreetype-dev
	avahi-daemon adb
)

do_deps=1
do_server=1
do_headset=1
headset_ip=""

while [ $# -gt 0 ]; do
	case "$1" in
		--skip-deps) do_deps=0 ;;
		--server-only) do_headset=0 ;;
		--headset-only) do_deps=0; do_server=0 ;;
		--headset) headset_ip="${2:?--headset needs the IP address of the headset}"; shift ;;
		-h | --help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
	esac
	shift
done

step() { printf '\n\033[1;33m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf '\n\033[1;31mError:\033[0m %s\n' "$*" >&2; exit 1; }

# Downloads $1 to $2 and checks it against the sha256 in $3.
fetch() {
	curl -fL --retry 3 -o "$2" "$1" || fail "could not download $1"
	echo "$3  $2" | sha256sum --check --quiet || fail "checksum mismatch for $1"
}

version_at_least() {
	[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]
}

install_deps() {
	step "Installing build dependencies (needs your password)"
	command -v apt-get >/dev/null ||
		fail "this installer uses apt-get (Ubuntu, Pop!_OS, Debian). Install the equivalents of these packages, then rerun with --skip-deps: ${APT_PACKAGES[*]}"
	sudo apt-get update
	sudo apt-get install -y "${APT_PACKAGES[@]}"
}

# Uses the system CMake when it's new enough, otherwise a pinned release in .tools
find_cmake() {
	if command -v cmake >/dev/null && version_at_least "$(cmake --version | head -1 | awk '{print $3}')" 3.28; then
		CMAKE=cmake
		return
	fi
	CMAKE="$TOOLS/cmake-$CMAKE_VERSION-linux-x86_64/bin/cmake"
	if [ ! -x "$CMAKE" ]; then
		step "Downloading CMake $CMAKE_VERSION"
		mkdir -p "$TOOLS"
		fetch "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz" \
			"$TOOLS/cmake.tar.gz" "$CMAKE_SHA256"
		tar -xzf "$TOOLS/cmake.tar.gz" -C "$TOOLS"
		rm "$TOOLS/cmake.tar.gz"
	fi
}

# The server needs Vulkan 1.4 headers. Ubuntu 24.04 ships 1.3, so fetch pinned headers when needed.
find_vulkan_headers() {
	VULKAN_ARGS=()
	if grep -q "VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION(0, 1, [4-9]" /usr/include/vulkan/vulkan_core.h 2>/dev/null; then
		return
	fi
	local dir="$TOOLS/Vulkan-Headers-$VULKAN_HEADERS_TAG"
	if [ ! -d "$dir" ]; then
		step "Downloading Vulkan 1.4 headers"
		mkdir -p "$TOOLS"
		fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$VULKAN_HEADERS_TAG.tar.gz" \
			"$TOOLS/vulkan-headers.tar.gz" "$VULKAN_HEADERS_SHA256"
		tar -xzf "$TOOLS/vulkan-headers.tar.gz" -C "$TOOLS"
		rm "$TOOLS/vulkan-headers.tar.gz"
	fi
	VULKAN_ARGS=(-DVulkan_INCLUDE_DIR="$dir/include")
}

build_server() {
	find_cmake
	find_vulkan_headers

	step "Configuring the server"
	"$CMAKE" -S "$ROOT/source" -B "$BUILD" -G Ninja \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DWIVRN_BUILD_CLIENT=OFF \
		-DWIVRN_BUILD_DASHBOARD=OFF \
		-DWIVRN_BUILD_TEST=OFF \
		"${VULKAN_ARGS[@]}"

	step "Building the server (this takes a while the first time)"
	"$CMAKE" --build "$BUILD"

	step "Installing the server to $PREFIX"
	"$CMAKE" --install "$BUILD"
	install -Dm644 "$ROOT/logo.png" "$PREFIX/share/icons/lemon-pass-through.png"

	# Launcher command and menu entry. The server runs in a terminal window: close it to stop.
	install -Dm755 /dev/stdin "$HOME/.local/bin/lemon-pass-through" <<-EOF
		#!/bin/sh
		exec "$PREFIX/bin/wivrn-server" "\$@"
	EOF
	install -Dm644 /dev/stdin "$HOME/.local/share/applications/lemon-pass-through.desktop" <<-EOF
		[Desktop Entry]
		Type=Application
		Name=Lemon Pass Through
		Comment=Stream PC VR to your Quest with a passthrough window for your wheel
		Exec=$HOME/.local/bin/lemon-pass-through
		Icon=$PREFIX/share/icons/lemon-pass-through.png
		Terminal=true
		Categories=Game;
	EOF
}

find_adb() {
	for candidate in adb "$HOME/Android/platform-tools/adb" "$HOME/Android/Sdk/platform-tools/adb"; do
		if command -v "$candidate" >/dev/null; then
			ADB="$(command -v "$candidate")"
			return
		fi
	done
	return 1
}

install_headset_app() {
	step "Installing the headset app"
	if ! find_adb; then
		note "adb isn't installed, so the headset app was skipped."
		note "Install it with 'sudo apt install adb', then run: ./install.sh --headset-only"
		return
	fi

	if [ -n "$headset_ip" ]; then
		"$ADB" connect "$headset_ip:5555" || true
		export ANDROID_SERIAL="$headset_ip:5555"
	fi
	if ! "$ADB" get-state >/dev/null 2>&1; then
		note "No headset found. Plug the Quest in with USB (developer mode on, allow USB debugging"
		note "in the headset), or pass --headset <ip> for Wi-Fi. Then run: ./install.sh --headset-only"
		return
	fi

	local apk="$ROOT/LemonPassThrough.apk"
	if [ ! -f "$apk" ]; then
		note "Downloading the app from the latest GitHub release"
		curl -fL --retry 3 -o "$apk" "$APK_URL" || fail "could not download $APK_URL"
	fi
	if ! "$ADB" install -r "$apk"; then
		note "Install failed. If another WiVRn build with the same app ID is installed, remove it first:"
		note "  adb uninstall $APP_ID"
		fail "the headset app was not installed"
	fi
	note "Installed. It's in the headset's app library under Unknown Sources."
}

[ "$do_deps" = 1 ] && install_deps
[ "$do_server" = 1 ] && build_server
[ "$do_headset" = 1 ] && install_headset_app

step "Done"
if [ "$do_server" = 1 ]; then
	cat <<-EOF
	    Start the server: open "Lemon Pass Through" from your apps menu, or run lemon-pass-through.
	    In Steam, set the game's launch options to:
	        PRESSURE_VESSEL_FILESYSTEMS_RW=\$XDG_RUNTIME_DIR/wivrn/comp_ipc %command%
	    Then: start the server, connect from the headset app, and only then start the game.
	    Full guide: GUIDE.md
	EOF
fi
