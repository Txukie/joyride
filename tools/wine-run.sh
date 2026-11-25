#!/usr/bin/env bash
# joyride's Windows build, played under Wine (for testing it on Linux).
#
#   tools/wine-run.sh [GTA folder]
#
# The GTA folder: the launcher's (games/gta1/location) unless given. The
# build: build-win/w/joyride.exe (README, Windows), or JOYRIDE_WIN_EXE. Its
# own Wine prefix, ~/.local/share/joyride-wine (or WINEPREFIX), so nothing
# else's is touched; there joyride keeps its data as on Windows
# (%LOCALAPPDATA%\joyride: settings, saves, reports), given the menu
# pictures the launcher made. Extra JOYRIDE_* variables pass through.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
exe="${JOYRIDE_WIN_EXE:-$root/build-win/w/joyride.exe}"
data="${XDG_DATA_HOME:-$HOME/.local/share}/joyride"
gta="${1:-$(cat "$data/games/gta1/location" 2>/dev/null || true)}"
[ -f "$exe" ] || { echo "no $exe: build it first (README, Windows)"; exit 1; }
[ -d "$gta/WINO" ] || { echo "no GTA folder (with WINO) at '$gta': give it as the argument"; exit 1; }

# the DLLs beside the exe: SDL3's and SDL3_ttf's (from their MinGW
# development packages, build-win/deps) and MinGW's threads
dir="$(dirname "$exe")"
for f in "$root"/build-win/deps/SDL3-*/x86_64-w64-mingw32/bin/SDL3.dll \
         "$root"/build-win/deps/SDL3_ttf-*/x86_64-w64-mingw32/bin/SDL3_ttf.dll \
         /usr/x86_64-w64-mingw32/bin/libwinpthread-1.dll; do
    [ -f "$f" ] && [ ! -f "$dir/$(basename "$f")" ] && cp "$f" "$dir/"
done

export WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/joyride-wine}"
export WINEDEBUG="${WINEDEBUG:--all}"
if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "making the Wine prefix $WINEPREFIX (once, a minute)"
    wineboot -i >/dev/null 2>&1
fi

# the menu pictures, where the Windows build looks for them
local="$WINEPREFIX/drive_c/users/$USER/AppData/Local/joyride/extracted/menu"
mkdir -p "$local"
for p in "$data"/extracted/menu/*.png; do
    [ -f "$p" ] && cp -u "$p" "$local/"
done

echo "joyride.exe under Wine: $gta"
exec wine "$exe" "Z:$gta"
