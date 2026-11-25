#!/usr/bin/env bash
# joyride for Windows, without the game: what a Windows player's machine
# needs to make joyride from their own copy of GTA with nothing installed
# (tools/lift.py, then tools/link_game.py). The counterpart of
# tools/appimage.sh; the launcher comes on top of it.
#
#   tools/windows-bundle.sh [work folder]         (default build-win/)
#
# Makes <work>/bundle/joyride/:
#   runtime/     joyride's runtime, cross-compiled (no game code), SDL3's
#                import libraries and DLLs, link.json, and cc/: GCC for
#                Windows (WinLibs, MinGW-w64 UCRT), C only
#   python/      Python for Windows (the embeddable one) with the tools'
#                packages (tools/requirements.txt, Windows wheels)
#   src/         joyride's source (git's files)
#
# From <work>/deps/ (downloaded once, from their own releases): SDL3's and
# SDL3_ttf's MinGW development packages (unpacked), python-*-embed-amd64.zip,
# winlibs-*.zip, wheels/ (pip download --platform win_amd64). Needs MinGW-w64
# (x86_64-w64-mingw32-gcc), CMake, Ninja, glslc.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mkdir -p "${1:-$root/build-win}" && cd "${1:-$root/build-win}" && pwd)"
deps="$work/deps"
B="$work/bundle/joyride"
R="$B/runtime"
rm -rf "$B"
mkdir -p "$R" "$B/python" "$B/src"
one() { ls -d $1 2>/dev/null | sort -V | tail -1; }   # the newest of a pattern
sdl="$(one "$deps/SDL3-[0-9]*/x86_64-w64-mingw32")"
ttf="$(one "$deps/SDL3_ttf-[0-9]*/x86_64-w64-mingw32")"
[ -d "$sdl" ] && [ -d "$ttf" ] || { echo "SDL3 and SDL3_ttf's MinGW packages are not in $deps"; exit 1; }

echo "== joyride's runtime for Windows (no game code)"
cmake -S "$root" -B "$work/runtime" -G Ninja -DCMAKE_BUILD_TYPE=Release -DJOYRIDE_RUNTIME_ONLY=ON -DJOYRIDE_RELEASE=ON \
      -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64.cmake" -DCMAKE_PREFIX_PATH="$sdl;$ttf" -DJOYRIDE_WITH_FFMPEG=OFF >/dev/null
cmake --build "$work/runtime" --target joyride_runtime win32hle joyride_compat
cp "$work/runtime/"lib{joyride_runtime,win32hle,joyride_compat}.a "$R/"
if x86_64-w64-mingw32-nm --defined-only "$R"/*.a 2>/dev/null | grep -qE ' sub_[0-9A-F]{8}$'; then
    echo "the runtime has generated game code in it: not packaged"; exit 1
fi
cp "$sdl/lib/libSDL3.dll.a" "$ttf/lib/libSDL3_ttf.dll.a" "$sdl/bin/SDL3.dll" "$ttf/bin/SDL3_ttf.dll" "$R/"

echo "== GCC for Windows (C only)"
unzip -q -o "$(one "$deps/winlibs-*.zip")" -d "$work/winlibs"
cp -a "$work/winlibs/mingw64" "$R/cc"
rm -rf "$work/winlibs"
# only what compiling C and linking it with the runtime uses (WinLibs is
# a whole toolchain: half a gigabyte of other languages, tools and SDKs)
( cd "$R/cc"
  keep_exe="gcc.exe as.exe ld.exe ld.bfd.exe ar.exe"
  # the DLLs those load, and GCC's own parts load, followed through
  need=""
  todo="$(for e in $keep_exe; do echo bin/$e; done) $(ls libexec/gcc/*/*/*.exe)"
  while [ -n "$todo" ]; do
      next=""
      for f in $todo; do
          for d in $(x86_64-w64-mingw32-objdump -p "$f" 2>/dev/null | awk '/DLL Name/{print $3}'); do
              [ -f "bin/$d" ] && case " $need " in *" $d "*) ;; *) need="$need $d"; next="$next bin/$d";; esac
          done
      done
      todo="$next"
  done
  for f in bin/*; do
      n="$(basename "$f")"
      case " $keep_exe $need " in *" $n "*) ;; *) rm -rf "$f";; esac
  done
  rm -rf share opt etc doc lib/python* libexec/gcc/*/*/lto1.exe libexec/gcc/*/*/cc1plus.exe libexec/gcc/*/*/f951.exe \
         libexec/gcc/*/*/gnat1.exe libexec/gcc/*/*/cc1obj*.exe libexec/gcc/*/*/d21.exe libexec/gcc/*/*/go1.exe
  for f in lib/*; do [ "$(basename "$f")" = gcc ] || rm -rf "$f"; done
  T=x86_64-w64-mingw32
  for f in $T/bin/*; do case "$(basename "$f")" in as.exe|ld.exe|ld.bfd.exe|ar.exe) ;; *) rm -rf "$f";; esac; done
  cp -n bin/*.dll "$T/bin/"                      # (gcc runs these: Windows finds their DLLs, and theirs, beside them)
  # the libraries: the C runtime's, GCC's and those a link names
  for f in $T/lib/lib*; do
      case "$(basename "$f")" in
          libmingw32.a|libmingwex.a|libmingwthrd.a|libmoldname.a|libmsvcrt*.a|libucrt*.a|libapi-ms-win-crt*|libvcruntime*.a| \
          libkernel32.a|libuser32.a|libgdi32.a|libshell32.a|libole32.a|liboleaut32.a|libuuid.a|libadvapi32.a| \
          libws2_32.a|libwinmm.a|libm.a|libpthread*|libwinpthread*|libntdll.a|libkernelbase.a|libcomdlg32.a| \
          libwinspool.a|libssp*|libquadmath*|libbcrypt.a|libsetupapi.a|libimm32.a|libversion.a|libdxguid.a) ;;
          *) rm -rf "$f";;
      esac
  done
  rm -rf $T/include/ddk $T/include/GL $T/include/gdiplus $T/include/wrl $T/include/winrt $T/include/psdk_inc/intrin-impl.h.unused \
         $T/include/*.idl $T/include/mshtml* $T/include/d3d* $T/include/d2d* $T/include/dxgi* $T/include/dwrite* \
         $T/include/windows.*.h $T/include/c++ $T/include/*.tlb
  rm -rf include/c++ )

# how tools/link_game.py links the game here: the flags the game's own
# build has on Windows (CMakeLists.txt), its libraries in order, its DLLs
cat > "$R/link.json" <<'EOF'
{ "exe": "joyride.exe",
  "cflags": ["-Dweak=selectany", "-fcommon"],
  "libs": ["libjoyride_runtime.a", "libwin32hle.a", "libjoyride_compat.a", "libSDL3_ttf.dll.a", "libSDL3.dll.a",
           "-lws2_32", "-lwinmm", "-lm", "-lkernel32", "-luser32", "-lgdi32", "-lshell32", "-lole32", "-loleaut32",
           "-luuid", "-ladvapi32"],
  "ldflags": [],
  "beside": ["SDL3.dll", "SDL3_ttf.dll", "cc/bin/libwinpthread-1.dll"] }
EOF

echo "== Python for Windows"
unzip -q "$(one "$deps/python-*-embed-amd64.zip")" -d "$B/python"
# the usual rules for its path (its folder, Lib\site-packages, the current
# folder for -m), not the embeddable's fixed list
rm -f "$B/python/"python*._pth
site="$B/python/Lib/site-packages"
mkdir -p "$site"
for w in "$deps"/wheels/*.whl; do unzip -q -o "$w" -d "$site"; done

echo "== the source"
(cd "$root" && git ls-files --recurse-submodules -z | xargs -0 cp --parents -p -t "$B/src/")
version="$(sed -n 's/.*project(joyride_launcher VERSION \([0-9.]*\).*/\1/p' "$root/launcher/CMakeLists.txt")"
echo "$version $(git -C "$root" rev-parse --short HEAD)" > "$B/src/.joyride-version"

du -sh "$B" "$R/cc" "$B/python"
