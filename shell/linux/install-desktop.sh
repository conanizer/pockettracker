#!/bin/sh
#
# Adds PocketTracker to your desktop's application menu and launcher, with its icon:
#
#     ./install-desktop.sh            add it (run it again after moving this folder)
#     ./install-desktop.sh --remove   take it out again
#
# Only your own account is touched (~/.local/share), so no sudo.
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
APPS=$DATA/applications
ICONS=$DATA/icons/hicolor/256x256/apps

if [ "${1:-}" = "--remove" ]; then
    rm -f "$APPS/pockettracker.desktop" "$ICONS/pockettracker.png"
    echo "PocketTracker removed from the application menu."
    exit 0
fi

if [ ! -x "$DIR/PocketTracker" ]; then
    echo "PocketTracker is not in $DIR - run this script from the unpacked folder." >&2
    exit 1
fi

mkdir -p "$APPS" "$ICONS"
cp "$DIR/pockettracker.png" "$ICONS/pockettracker.png"

# ⚠️ The file name and StartupWMClass must match the app id the program gives its window
# (shell/main.cpp), or the taskbar shows a window with no icon.
cat > "$APPS/pockettracker.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=PocketTracker
Comment=Music tracker
Exec="$DIR/PocketTracker"
Path=$DIR
Icon=pockettracker
Terminal=false
Categories=AudioVideo;Audio;Music;
StartupWMClass=pockettracker
EOF

# Menus that cache their lists see the new entry sooner with these; neither is required.
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database -q "$APPS" || true; fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then gtk-update-icon-cache -q -t "$DATA/icons/hicolor" || true; fi

echo "PocketTracker added to the application menu ($APPS/pockettracker.desktop)."
