#!/usr/bin/env bash
# joyride for a player who has no build tools: the game's generated code
# (tools/lift.sh's gen/, made from their own executable) compiled and linked
# to joyride's runtime, which the AppImage carries prebuilt with the
# libraries it needs (tools/appimage.sh). The same flags as CMake's build.
#
#   tools/link-game.sh <the AppImage's usr/ folder> [out]
#
# CC: the compiler (the AppImage's own, cc/joyride-cc); out: build/joyride.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
usr="$(cd "$1" && pwd)"
out="${2:-$root/build/joyride}"
cc="${CC:-$usr/lib/joyride/cc/joyride-cc}"
gen="$root/gen/wino/recomp"
obj="$root/build/gen"
rt="$usr/lib/joyride"

ls "$gen"/*.c >/dev/null || { echo "no generated code in $gen: run tools/lift.sh first"; exit 1; }
mkdir -p "$obj" "$(dirname "$out")"

# the lifted game: -O1, quietly, as CMakeLists.txt's joyride_gen
echo "compiling the game ($(ls "$gen"/*.c | wc -l) files)"
ls "$gen"/*.c | xargs -P "$(nproc)" -I{} sh -c \
  'f="{}"; o="$4/$(basename "$f" .c).o"; "$0" -O1 -w -DRECOMP_MEM_BASE=0x100000000000ull \
     -I "$1/external/pcrecomp/runtime/recomp32" -I "$2" \
     -include "$2/recomp_funcs.h" -include "$1/src/hle/texfilter_inline.h" -c "$f" -o "$o" && echo "  $(basename "$f")"' \
  "$cc" "$root" "$gen" "$rt" "$obj"

# joyride's runtime, then the libraries it was built against (by path: the
# AppImage's copies), and what of the system's they need found at run time
echo "linking $out"
lib() { ls "$usr"/lib/$1 2>/dev/null | head -1; }      # $1 a pattern
"$cc" -o "$out" "$obj"/*.o "$rt/libjoyride_runtime.a" "$rt/libwin32hle.a" \
  "$(lib 'libSDL3_ttf.so.*')" "$(lib 'libSDL3.so.*')" \
  "$(lib 'libavformat.so.*')" "$(lib 'libavcodec.so.*')" "$(lib 'libavutil.so.*')" \
  -lm -lpthread -ldl -Wl,-rpath-link,"$usr/lib" -Wl,--allow-shlib-undefined
echo "joyride ready: $out"
