#!/usr/bin/env python3
"""Unpack the original executable into analysis artefacts.

    python3 tools/unpack.py "game_data/GTAWIN/GRAND THEFT AUTO.EXE" gen/analysis

Writes (all derived from the game binary, so all under the git-ignored gen/):
    image.bin      the image mapped as in memory; offset 0 == ImageBase
    pe.json        headers, sections, data directories, resources
    imports.json   every import with the VA of its IAT slot
    relocs.json    every HIGHLOW fixup: where it is and what it points to
and prints a human-readable summary.
"""

from __future__ import annotations

import collections
import datetime as dt
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe.pe import DIR_NAMES, parse  # noqa: E402

KNOWN = {
    "038561860ce80eda794a491935d64878f925376e": "GTA 1997 CD (Windows, 1997-10-10)",
    "d36976d66599cd649bf24f8ccd45bda84c0ee3f8": "GTA Steam (Windows, 2002-11-11)",
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    path, out = sys.argv[1], sys.argv[2]
    data = open(path, "rb").read()
    sha1 = hashlib.sha1(data).hexdigest()
    pe = parse(data)
    os.makedirs(out, exist_ok=True)

    img = pe.map_image()
    open(os.path.join(out, "image.bin"), "wb").write(img)
    base = pe.image_base

    def sec_name(va: int) -> str | None:
        s = pe.section_for_rva(va - base)
        return s.name if s else None

    # Relocation targets: every fixup holds a pointer; classify where it points.
    reloc_rows = []
    by_pair: collections.Counter[tuple[str, str]] = collections.Counter()
    code_targets: set[int] = set()
    text = next(s for s in pe.sections if s.name == ".text")
    for va in pe.relocs:
        tgt = struct.unpack_from("<I", img, va - base)[0]
        src, dst = sec_name(va), sec_name(tgt) if base <= tgt < base + pe.size_of_image else None
        reloc_rows.append({"at": va, "target": tgt, "in": src, "to": dst})
        by_pair[(src or "?", dst or "outside")] += 1
        if dst == ".text":
            code_targets.add(tgt)

    iat_slots = {imp.iat_va for imp in pe.imports}

    json.dump({
        "file": os.path.basename(path), "sha1": sha1, "build": KNOWN.get(sha1),
        "machine": pe.machine, "timestamp": pe.timestamp,
        "characteristics": pe.characteristics, "image_base": base,
        "entry": base + pe.entry_rva, "size_of_image": pe.size_of_image,
        "subsystem": pe.subsystem, "linker": pe.linker, "os_version": pe.os_version,
        "stack": pe.stack, "heap": pe.heap,
        "sections": [{"name": s.name, "va": base + s.vaddr, "vsize": s.vsize,
                      "raw_off": s.raw_off, "raw_size": s.raw_size,
                      "flags": s.flags, "perms": s.perms} for s in pe.sections],
        "dirs": {DIR_NAMES[i]: {"va": base + r, "size": n}
                 for i, (r, n) in enumerate(pe.dirs) if r},
        "exports": [e.__dict__ for e in pe.exports],
        "resources": [{"type": t, "name": n, "va": base + r, "size": z}
                      for t, n, r, z in pe.resources],
    }, open(os.path.join(out, "pe.json"), "w"), indent=1)
    json.dump([i.__dict__ for i in pe.imports],
              open(os.path.join(out, "imports.json"), "w"), indent=1)
    json.dump({"highlow": reloc_rows,
               "other": [{"type": t, "at": v} for t, v in pe.reloc_other]},
              open(os.path.join(out, "relocs.json"), "w"))

    # ---- summary ---------------------------------------------------------
    p = print
    p(f"file        {os.path.basename(path)}  ({len(data):,} bytes)")
    p(f"sha1        {sha1}  {KNOWN.get(sha1, 'UNKNOWN BUILD')}")
    p(f"timestamp   {dt.datetime.fromtimestamp(pe.timestamp, dt.UTC):%Y-%m-%d %H:%M:%S} UTC")
    p(f"linker      {pe.linker[0]}.{pe.linker[1]}   subsystem {pe.subsystem} "
      f"({'GUI' if pe.subsystem == 2 else 'console' if pe.subsystem == 3 else '?'})")
    p(f"image base  {base:#010x}   entry {base + pe.entry_rva:#010x}   size {pe.size_of_image:#x}")
    p(f"stack       reserve {pe.stack[0]:#x} commit {pe.stack[1]:#x}   "
      f"heap reserve {pe.heap[0]:#x} commit {pe.heap[1]:#x}")
    p("\nsections")
    for s in pe.sections:
        p(f"  {s.name:8} {base + s.vaddr:#010x}  vsize {s.vsize:#9x}  raw {s.raw_size:#9x}  {s.perms}")
    p("\ndata directories")
    for i, (r, n) in enumerate(pe.dirs):
        if r:
            p(f"  {DIR_NAMES[i]:14} {base + r:#010x}  {n:#x}  ({sec_name(base + r)})")

    p(f"\nimports: {len(pe.imports)} functions from {len({i.dll.lower() for i in pe.imports})} DLLs")
    per = collections.OrderedDict()
    for imp in pe.imports:
        per.setdefault(imp.dll, []).append(imp)
    for dll, lst in per.items():
        names = [i.name or f"#{i.ordinal}" for i in lst]
        p(f"  {dll} ({len(lst)})")
        line = "    "
        for nm in names:
            if len(line) + len(nm) > 100:
                p(line)
                line = "    "
            line += nm + " "
        p(line)

    p(f"\nexports: {len(pe.exports)}")
    for e in pe.exports:
        p(f"  #{e.ordinal} {e.name} {base + e.rva:#x}")

    p(f"\nrelocations: {len(pe.relocs):,} HIGHLOW fixups"
      + (f", {len(pe.reloc_other)} other" if pe.reloc_other else ""))
    for (a, b), n in sorted(by_pair.items(), key=lambda kv: -kv[1]):
        p(f"  in {a:7} -> {b:8} {n:7,}")
    in_text_to_text = [r for r in reloc_rows if r["in"] == ".text" and r["to"] == ".text"]
    iat_refs = sum(1 for r in reloc_rows if r["target"] in iat_slots)
    p(f"  distinct code addresses referenced by pointer: {len(code_targets):,}")
    p(f"  fixups pointing at IAT slots (calls/jumps through imports): {iat_refs:,}")
    p(f"  code->code pointers inside .text (jump tables, push-offset): {len(in_text_to_text):,}")

    p(f"\nresources: {len(pe.resources)}")
    for t, n, r, z in pe.resources:
        p(f"  {t:12} {n:20} {z:6} bytes")

    p(f"\nwrote {out}/image.bin pe.json imports.json relocs.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
