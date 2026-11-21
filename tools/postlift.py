#!/usr/bin/env python3
"""Post-pass over pcrecomp's generated C (tools/lift.sh runs it).

    python3 tools/postlift.py gen/wino/recomp

Adds RECOMP_BACKEDGE() before every branch back to an earlier instruction,
i.e. every loop. pcrecomp's generate.py does this; the default translator
(`python -m tools`) does not. Without it a loop that spins on a variable
another guest thread updates never lets that thread run: GTA's frame limiter
(0x0047DC96) waits on a counter its Miles timer callback advances, and the
timer thread waits for the machine the spinning loop holds.

The generated lines carry the instruction's address in their comment
(`/* 0x0047DC9C: jb 0x47dc96 */`), so a backward goto is recognisable from
the text alone. Idempotent: a line already preceded by RECOMP_BACKEDGE()
is left alone.
"""

import os
import re
import sys

GOTO = re.compile(r"goto L_([0-9A-F]{8});.*/\* 0x([0-9A-F]{8}): (j\w*|loop\w*) ")


def process(path: str) -> int:
    with open(path) as f:
        lines = f.readlines()
    out, added = [], 0
    for line in lines:
        m = GOTO.search(line)
        if m and int(m.group(1), 16) <= int(m.group(2), 16):
            prev = out[-1] if out else ""
            if "RECOMP_BACKEDGE()" not in prev:
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f"{indent}RECOMP_BACKEDGE();\n")
                added += 1
        out.append(line)
    if added:
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.writelines(out)
        os.replace(tmp, path)
    return added


def main() -> int:
    d = sys.argv[1] if len(sys.argv) > 1 else "gen/wino/recomp"
    total = 0
    for name in sorted(os.listdir(d)):
        if name.startswith("recomp_0") and name.endswith(".c"):
            total += process(os.path.join(d, name))
    print(f"postlift: {total} loop back-edges given a RECOMP_BACKEDGE()")
    return 0


if __name__ == "__main__":
    sys.exit(main())
