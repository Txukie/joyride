#!/usr/bin/env bash
# Translate the game executable to C with pcrecomp. Everything it writes is
# derived from the game binary and lands in the git-ignored gen/ directory.
#
#   tools/lift.sh [path/to/Grand Theft Auto.exe]
#
# Default input: game_data/WINO/Grand Theft Auto.exe (Steam or the free
# Rockstar Classics release; SHA-1 d36976d66599cd649bf24f8ccd45bda84c0ee3f8).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
exe="$(realpath "${1:-$root/game_data/WINO/Grand Theft Auto.exe}")"
py="$root/.venv/bin/python"
out="$root/gen/wino"
pcr="$root/external/pcrecomp"

[ -x "$py" ] || { echo "run: python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt"; exit 1; }
[ -f "$pcr/tools/__main__.py" ] || { echo "run: git submodule update --init"; exit 1; }

want=d36976d66599cd649bf24f8ccd45bda84c0ee3f8
got=$(sha1sum "$exe" | cut -d' ' -f1)
[ "$got" = "$want" ] || echo "warning: $exe is not the 2002 release (sha1 $got)"

mkdir -p "$out"
python3 "$root/tools/unpack.py" "$exe" "$out/mine" > "$out/unpack.txt"
"$py" "$root/tools/funcs.py" "$out/mine" > "$out/funcs.txt"
(cd "$pcr" && "$py" -m tools "$exe" --all -o "$out/recomp") > "$out/lift.log" 2>&1
tail -4 "$out/lift.log"
python3 "$root/tools/postlift.py" "$out/recomp"

# every generated file must at least parse, as the build compiles it: the
# runtime headers, and joyride's own (the splices postlift.py adds use it)
ls "$out"/recomp/*.c | xargs -P "$(nproc)" -I{} sh -c \
  'LC_ALL=C gcc -w -fsyntax-only -DRECOMP_MEM_BASE=0x100000000000ull -I "$0/runtime/recomp32" -I "$2/src/hle" -include "$1/recomp/recomp_funcs.h" -include "$2/src/hle/texfilter_inline.h" "{}"' \
  "$pcr" "$out" "$root"
echo "all generated files parse"
