#!/usr/bin/env bash
# Removes what install.sh installed on the PC. Your saved wheel position (~/.config/wivrn/wheel-mask.txt) is kept.
#
#   ./uninstall.sh            remove the server, launcher and menu entry
#   ./uninstall.sh --headset  also remove the app from a connected headset
set -euo pipefail

PREFIX="${LEMON_PREFIX:-$HOME/.local/share/lemon-pass-through}"
APP_ID="io.github.j99droid.lemonpassthrough"

rm -rf "$PREFIX"
rm -f "$HOME/.local/bin/lemon-pass-through" "$HOME/.local/share/applications/lemon-pass-through.desktop"
echo "Removed the Lemon Pass Through server and launcher."

if [ "${1:-}" = "--headset" ]; then
	adb uninstall "$APP_ID" && echo "Removed the headset app."
fi
