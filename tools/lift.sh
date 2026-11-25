#!/usr/bin/env bash
# Translate the game executable to C with pcrecomp (tools/lift.py, with the
# checkout's .venv). Everything it writes is derived from the game binary
# and lands in the git-ignored gen/ directory.
#
#   tools/lift.sh [path/to/Grand Theft Auto.exe]
#
# Default input: game_data/WINO/Grand Theft Auto.exe (Steam or the free
# Rockstar Classics release; SHA-1 d36976d66599cd649bf24f8ccd45bda84c0ee3f8).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
py="${JOYRIDE_PYTHON:-$root/.venv/bin/python}"
[ -x "$py" ] || { echo "run: python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt"; exit 1; }
exec "$py" "$root/tools/lift.py" "${1:-$root/game_data/WINO/Grand Theft Auto.exe}" ${CC:+--cc "$CC"}
