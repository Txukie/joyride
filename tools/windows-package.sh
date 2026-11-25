#!/usr/bin/env bash
# joyride for Windows, the package a player unzips: the launcher, with Qt,
# and the bundle (tools/windows-bundle.sh: joyride's runtime, Python and
# GCC to make joyride from the player's own GTA). Nothing of the game.
#
#   tools/windows-package.sh [work folder]        (default build-win/)
#
# Makes <work>/package/joyride/ and <work>/joyride-<version>-windows-x64.zip:
#   joyride-launcher.exe, joyride-setup.exe, the DLLs they load (Qt,
#   libarchive, SDL3, ...), plugins/ and qml/ (what the launcher's QML
#   imports), qt.conf, and the bundle's src/, runtime/, python/.
#
# Needs: the launcher cross-compiled (cmake -S launcher -B <work>/launcher
# with cmake/mingw-w64.cmake, CMAKE_PREFIX_PATH=<work>/msys/root/ucrt64,
# QT_HOST_PATH=/usr), the bundle (<work>/bundle), MSYS2's Qt in <work>/msys.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(cd "${1:-$root/build-win}" && pwd)"
ucrt="$work/msys/root/ucrt64"
P="$work/package/joyride"
version="$(sed -n 's/.*project(joyride_launcher VERSION \([0-9.]*\).*/\1/p' "$root/launcher/CMakeLists.txt")"
[ -f "$work/launcher/joyride-launcher.exe" ] || { echo "the launcher is not built in $work/launcher"; exit 1; }
[ -d "$work/bundle/joyride/runtime" ] || { echo "no bundle: run tools/windows-bundle.sh"; exit 1; }
rm -rf "$P"
mkdir -p "$P/plugins" "$P/qml/QtQuick/Controls" "$P/qml/Qt/labs"

echo "== the launcher"
cp "$work/launcher/joyride-launcher.exe" "$work/launcher/joyride-setup.exe" "$P/"
# Qt's plugins: the Windows platform (and offscreen, for checking it
# without a screen), image formats, the style hints
q="$ucrt/share/qt6"
mkdir -p "$P/plugins/platforms"
cp "$q/plugins/platforms/qwindows.dll" "$q/plugins/platforms/qoffscreen.dll" "$P/plugins/platforms/"
cp -r "$q/plugins/imageformats" "$P/plugins/"
[ -d "$q/plugins/styles" ] && cp -r "$q/plugins/styles" "$P/plugins/"
# the QML modules its QML imports: QtQuick (Controls in their Basic style),
# Layouts, Window, QtQml, Qt.labs.folderlistmodel
cp -r "$q/qml/QtQml" "$P/qml/"
cp -r "$q/qml/Qt/labs/folderlistmodel" "$P/qml/Qt/labs/"
( cd "$q/qml/QtQuick" && cp -r qmldir plugins.qmltypes qtquick2plugin.dll Layouts Window Templates "$P/qml/QtQuick/" )
( cd "$q/qml/QtQuick/Controls" && cp -r qmldir plugins.qmltypes qtquickcontrols2plugin.dll Basic impl "$P/qml/QtQuick/Controls/" )
printf '[Paths]\nPrefix = .\nPlugins = plugins\nQmlImports = qml\nQml2Imports = qml\n' > "$P/qt.conf"

# the DLLs all of those load, and theirs, followed through their imports
# (what Windows has itself is not in MSYS2's bin, so the search stops)
todo="$(cd "$P" && find . -name '*.exe' -o -name '*.dll' | sed 's|^\./||')"
seen=" "
while [ -n "$todo" ]; do
    next=""
    for f in $todo; do
        for d in $(x86_64-w64-mingw32-objdump -p "$P/$f" 2>/dev/null | awk '/DLL Name/{print $3}'); do
            case "$seen" in *" $d "*) continue;; esac
            seen="$seen$d "
            if [ -f "$ucrt/bin/$d" ] && [ ! -f "$P/$d" ]; then cp "$ucrt/bin/$d" "$P/"; next="$next $d"; fi
        done
    done
    todo="$next"
done

echo "== the bundle"
cp -r "$work/bundle/joyride/src" "$work/bundle/joyride/runtime" "$work/bundle/joyride/python" "$P/"
rm -rf "$P/src/gen" "$P/src/build"                   # (a test's, if one was made there)

echo "== the zip"
zip="$work/joyride-$version-windows-x64.zip"
rm -f "$zip"
(cd "$work/package" && zip -qr -9 "$zip" joyride)
du -sh "$P"; ls -la "$zip"
