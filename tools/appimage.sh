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
cmake -S "$root" -B "$work/runtime" -G Ninja -DCMAKE_BUILD_TYPE=Release -DJOYRIDE_RUNTIME_ONLY=ON -DJOYRIDE_RELEASE=ON >/dev/null
cmake --build "$work/runtime" --target joyride_runtime win32hle
cp "$work/runtime/libjoyride_runtime.a" "$work/runtime/libwin32hle.a" "$J/"
if nm --defined-only "$J"/*.a 2>/dev/null | grep -qE ' sub_[0-9A-F]{8}$'; then
    echo "the runtime has generated game code in it: not packaged"; exit 1
fi

echo "== the launcher"
cmake -S "$root/launcher" -B "$work/launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/launcher"
install -m755 "$work/launcher/joyride-launcher" "$work/launcher/joyride-setup" "$A/usr/bin/"

echo "== the source"
mkdir -p "$A/usr/share/joyride/src"
(cd "$root" && git ls-files --recurse-submodules -z | xargs -0 cp --parents -p -t "$A/usr/share/joyride/src/")
echo "$version $(git -C "$root" rev-parse --short HEAD)" > "$A/usr/share/joyride/src/.joyride-version"

echo "== Python"
P="$J/python"
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

echo "== the compiler"
C="$J/cc"
triple="$(gcc -dumpmachine)"; gver="$(gcc -dumpversion)"
gdir="$(dirname "$(gcc -print-prog-name=cc1)")"
G="$C/lib/gcc/$triple/$gver"
mkdir -p "$C/bin" "$G" "$C/sysroot/usr/lib" "$C/sysroot/usr/include"
cp "$(readlink -f "$(command -v gcc)")" "$C/bin/gcc"
for f in cc1 collect2 lto-wrapper liblto_plugin.so crtbegin.o crtbeginS.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so \
         libgcc_s_asneeded.so libatomic_asneeded.so; do
    cp -a "$gdir/$f" "$G/"
done
cp -a "$gdir/include" "$G/"
cp "$(readlink -f /usr/bin/as)" "$G/as"
cp "$(readlink -f /usr/bin/ld.bfd)" "$G/ld"
# glibc's headers and link files, and the kernel's headers
pacman -Qlq glibc linux-api-headers | grep '^/usr/include/.' | while read -r f; do
    [ -d "$f" ] && mkdir -p "$C/sysroot$f" || cp -a "$f" "$C/sysroot$f"
done
for f in Scrt1.o crt1.o crti.o crtn.o libc.so libc.so.6 libc_nonshared.a libm.so libm.so.6 libmvec.so.1 \
         ld-linux-x86-64.so.2 libpthread.a libpthread.so.0 libdl.a libdl.so.2 librt.a libgcc_s.so.1 libatomic.so libatomic.so.1; do
    cp -a "$(readlink -f "/usr/lib/$f")" "$C/sysroot/usr/lib/$f"
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
    soname="$(objdump -p "/usr/lib/$l" | sed -n 's/^ *SONAME *//p')"
    libs+=(-l "/usr/lib/$soname")
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
qtplugins="$(qmake6 -query QT_INSTALL_PLUGINS)"
mkdir -p "$A/usr/plugins"
for d in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    cp -a "$qtplugins/$d" "$A/usr/plugins/"
    libs+=(--deploy-deps-only "$A/usr/plugins/$d")
done
# the AppImage's starter (AppImage's type2-runtime), fetched once and kept
rtfile="$work/runtime-x86_64"
[ -s "$rtfile" ] || curl -fsSL -o "$rtfile" https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
LDAI_RUNTIME_FILE="$rtfile" \
EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so" \
    NO_STRIP=1 PATCHELF="$(command -v patchelf)" QML_SOURCES_PATHS="$root/launcher/qml" QMAKE="$(command -v qmake6)" LINUXDEPLOY_OUTPUT_VERSION="$version" ARCH=x86_64 \
    linuxdeploy --appdir "$A" --plugin qt --output appimage \
        -e "$A/usr/bin/joyride-launcher" -e "$A/usr/bin/joyride-setup" "${libs[@]}" \
        --deploy-deps-only "$P" --deploy-deps-only "$C" \
        -d "$A/usr/share/applications/joyride.desktop" -i "$A/usr/share/icons/hicolor/256x256/apps/joyride.png"
ls -la "$work"/*.AppImage
