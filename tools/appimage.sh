#!/usr/bin/env bash
# joyride's AppImage: the launcher, and all a player's machine needs to make
# joyride from their own copy of GTA with nothing installed. Nothing of the
# game is in it: joyride's runtime comes prebuilt, without the generated
# code, which the launcher makes on the player's machine (tools/lift.sh)
# and links to it (tools/link_game.py) with the compiler carried here.
#
#   tools/appimage.sh [work folder]       (default build-appimage/)
#
# Carried, from this machine's own packages:
#   usr/bin/joyride-launcher, joyride-setup     the launcher (Qt, by linuxdeploy's Qt plugin)
#   usr/lib/joyride/libjoyride_runtime.a ...    joyride's runtime, no game in it
#   usr/lib/libSDL3*.so, libav*.so ...          what joyride links to, and theirs
#   usr/lib/joyride/python/                     Python, its standard library, and
#                                               capstone, unicorn, pefile, numpy, Pillow
#   usr/lib/joyride/cc/                         GCC (C only), binutils' as and ld, and
#                                               glibc's headers and link files (sysroot/)
#   usr/share/joyride/src/                      joyride's source (git's files), copied out
#                                               to build in (the AppImage is read-only)
#
# Needs: the usual build (CMake, Ninja, SDL3, ffmpeg, Qt 6), patchelf, linuxdeploy and
# linuxdeploy-plugin-qt, and the checkout's .venv (tools/requirements.txt).
# A player's glibc must be as new as this machine's or newer.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mkdir -p "${1:-$root/build-appimage}" && cd "${1:-$root/build-appimage}" && pwd)"
version="$(sed -n 's/.*project(joyride_launcher VERSION \([0-9.]*\).*/\1/p' "$root/launcher/CMakeLists.txt")"
A="$work/AppDir"
J="$A/usr/lib/joyride"
rm -rf "$A"
mkdir -p "$A/usr/bin" "$J" "$A/usr/share/joyride" "$A/usr/share/applications" "$A/usr/share/icons/hicolor/256x256/apps"

echo "== joyride's runtime (no game code)"
cmake -S "$root" -B "$work/runtime" -G Ninja -DCMAKE_BUILD_TYPE=Release -DJOYRIDE_RUNTIME_ONLY=ON -DJOYRIDE_RELEASE=ON \
      ${JOYRIDE_PREBUILT_SHADERS:+-DJOYRIDE_PREBUILT_SHADERS="$JOYRIDE_PREBUILT_SHADERS"} >/dev/null
cmake --build "$work/runtime" --target joyride_runtime win32hle
cp "$work/runtime/libjoyride_runtime.a" "$work/runtime/libwin32hle.a" "$J/"
if nm --defined-only "$J"/*.a 2>/dev/null | grep -qE ' sub_[0-9A-F]{8}$'; then
    echo "the runtime has generated game code in it: not packaged"; exit 1
fi

echo "== the launcher"
cmake -S "$root/launcher" -B "$work/launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release ${QT_DIR:+-DCMAKE_PREFIX_PATH="$QT_DIR"} >/dev/null
cmake --build "$work/launcher"
install -m755 "$work/launcher/joyride-launcher" "$work/launcher/joyride-setup" "$A/usr/bin/"

echo "== the source"
mkdir -p "$A/usr/share/joyride/src"
(cd "$root" && git ls-files --recurse-submodules -z | xargs -0 cp --parents -p -t "$A/usr/share/joyride/src/")
echo "$version $(git -C "$root" rev-parse --short HEAD)" > "$A/usr/share/joyride/src/.joyride-version"

echo "== Python"
P="$J/python"
if [ -n "${JOYRIDE_PYTHON_HOME:-}" ]; then
    # a relocatable Python with the tools' packages (the release container's,
    # python-build-standalone): copied whole, less pip, tests and the GUI
    cp -a "$JOYRIDE_PYTHON_HOME" "$P"
    rm -rf "$P"/lib/python3*/{test,idlelib,tkinter,turtledemo,ensurepip} "$P"/lib/python3*/site-packages/pip* "$P"/share "$P"/include
    find "$P" -name __pycache__ -prune -exec rm -rf {} +
else
    # this machine's Python and the checkout's .venv packages
    pyver="$("$root/.venv/bin/python" -c 'import sys; print(f"{sys.version_info[0]}.{sys.version_info[1]}")')"
    pybin="$(readlink -f "$(command -v "python$pyver")")"
    stdlib="$(python$pyver -c 'import sysconfig; print(sysconfig.get_paths()["stdlib"])')"
    mkdir -p "$P/bin" "$P/lib"
    cp "$pybin" "$P/bin/python3"
    rsync -a --exclude site-packages --exclude test --exclude tests --exclude idlelib --exclude tkinter \
          --exclude turtledemo --exclude ensurepip --exclude '__pycache__' "$stdlib/" "$P/lib/python$pyver/"
    site="$P/lib/python$pyver/site-packages"
    mkdir -p "$site"
    for p in capstone unicorn pefile.py peutils.py ordlookup numpy numpy.libs PIL pillow.libs; do
        src="$root/.venv/lib/python$pyver/site-packages/$p"
        [ -e "$src" ] && cp -a "$src" "$site/"
    done
fi

echo "== the compiler"
C="$J/cc"
triple="$(gcc -dumpmachine)"; gver="$(gcc -dumpversion)"
gdir="$(dirname "$(gcc -print-prog-name=cc1)")"
G="$C/lib/gcc/$triple/$gver"
mkdir -p "$C/bin" "$G" "$C/sysroot/usr/lib" "$C/sysroot/usr/include"
cp "$(readlink -f "$(command -v gcc)")" "$C/bin/gcc"
for f in cc1 collect2 lto-wrapper liblto_plugin.so crtbegin.o crtbeginS.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so \
         libgcc_s_asneeded.so libatomic_asneeded.so; do
    [ -e "$gdir/$f" ] && cp -a "$gdir/$f" "$G/"            # (the last two: GCC 16's)
done
cp -a "$gdir/include" "$G/"
cp "$(readlink -f /usr/bin/as)" "$G/as"
cp "$(readlink -f /usr/bin/ld.bfd)" "$G/ld"
# glibc's headers and link files, and the kernel's headers, where this
# system keeps them (Arch: /usr/lib; Ubuntu: /usr/lib/x86_64-linux-gnu), at
# the same paths in the sysroot; and what glibc's link scripts name
if command -v pacman >/dev/null; then hdrs="$(pacman -Qlq glibc linux-api-headers)"
else hdrs="$(dpkg -L libc6-dev linux-libc-dev)"; fi
echo "$hdrs" | grep '^/usr/include/.' | while read -r f; do
    if [ -d "$f" ]; then mkdir -p "$C/sysroot$f"; else mkdir -p "$C/sysroot$(dirname "$f")"; cp -a "$f" "$C/sysroot$f"; fi
done
sysfile() { [ -e "$1" ] || return 0; mkdir -p "$C/sysroot$(dirname "$1")"; cp -L "$1" "$C/sysroot$1"; }
libdir="$(dirname "$(gcc -print-file-name=libc.so)")"
for f in Scrt1.o crt1.o crti.o crtn.o libc.so libc.so.6 libc_nonshared.a libm.so libm.so.6 libmvec.so libmvec.so.1 \
         ld-linux-x86-64.so.2 libpthread.a libpthread.so.0 libdl.a libdl.so.2 librt.a libgcc_s.so.1 libatomic.so libatomic.so.1; do
    sysfile "$libdir/$f"
done
for s in libc.so libm.so; do
    for f in $(grep -oE '/[^ )]+' "$libdir/$s" | grep -v '^/\*'); do sysfile "$f"; done
done
cat > "$C/joyride-cc" <<'EOF'
#!/bin/sh
# the AppImage's GCC: its parts beside it, glibc's files in sysroot/
d="$(cd "$(dirname "$0")" && pwd)"
LD_LIBRARY_PATH="$d/../..${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$d/bin/gcc" --sysroot="$d/sysroot" "$@"
EOF
chmod +x "$C/joyride-cc"

echo "== the desktop entry and icon"
cp "$root/art/joyride-256.png" "$A/usr/share/icons/hicolor/256x256/apps/joyride.png"
cat > "$A/usr/share/applications/joyride.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=joyride
Comment=Grand Theft Auto, London 1969 and 1961, recompiled
Exec=joyride-launcher
Icon=joyride
Terminal=false
Categories=Game;
StartupWMClass=joyride
X-AppImage-Version=$version
EOF

echo "== libraries, Qt, the AppImage"
libs=()
linked=()
for l in libSDL3_ttf.so libSDL3.so libavformat.so libavcodec.so libavutil.so; do   # by the name programs load them by
    at="$(gcc -print-file-name="$l")"                     # wherever this system has it: GCC's paths,
    case "$at" in */*) ;; *) at="$(ldconfig -p | awk -v n="$l" '$1 == n { print $NF; exit }')";; esac   # else the loader's (/usr/local/lib)
    at="$(readlink -f "$at")"
    soname="$(objdump -p "$at" | sed -n 's/^ *SONAME *//p')"
    libs+=(-l "$(dirname "$at")/$soname")
    linked+=("\"../$soname\"")
done
# how tools/link_game.py links the game to the runtime here
cat > "$J/link.json" <<EOF
{ "exe": "joyride",
  "cflags": [],
  "libs": ["libjoyride_runtime.a", "libwin32hle.a", $(IFS=,; echo "${linked[*]}"), "-lm", "-lpthread", "-ldl"],
  "ldflags": ["-Wl,-rpath-link,{rt}/..", "-Wl,--allow-shlib-undefined"] }
EOF
cd "$work"
# Wayland's own plugins beside the platform one (xdg-shell, decorations, its GL)
QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
qtplugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
mkdir -p "$A/usr/plugins"
for d in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    cp -a "$qtplugins/$d" "$A/usr/plugins/"
    libs+=(--deploy-deps-only "$A/usr/plugins/$d")
done
# libraries linuxdeploy leaves to the system (its exclude list: FreeType,
# HarfBuzz) that this build carries anyway (JOYRIDE_BUNDLE_LIBS: the release
# container's own, newer than any system's), by their sonames
mkdir -p "$A/usr/lib"
for f in ${JOYRIDE_BUNDLE_LIBS:-}; do cp -L "$f" "$A/usr/lib/$(basename "$f")"; done
# the AppImage's starter (AppImage's type2-runtime), fetched once and kept
rtfile="$work/runtime-x86_64"
[ -s "$rtfile" ] || curl -fsSL -o "$rtfile" https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
LDAI_RUNTIME_FILE="$rtfile" \
EXTRA_PLATFORM_PLUGINS="$(cd "$qtplugins/platforms" && ls libqwayland*.so | tr '\n' ';')libqoffscreen.so" \
    NO_STRIP=1 PATCHELF="$(command -v patchelf)" QML_SOURCES_PATHS="$root/launcher/qml" QMAKE="$QMAKE" LINUXDEPLOY_OUTPUT_VERSION="$version" ARCH=x86_64 \
    "${LINUXDEPLOY:-linuxdeploy}" --appdir "$A" --plugin qt --output appimage \
        -e "$A/usr/bin/joyride-launcher" -e "$A/usr/bin/joyride-setup" "${libs[@]}" \
        --deploy-deps-only "$P" --deploy-deps-only "$C" \
        -d "$A/usr/share/applications/joyride.desktop" -i "$A/usr/share/icons/hicolor/256x256/apps/joyride.png"
ls -la "$work"/*.AppImage
