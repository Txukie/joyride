#!/usr/bin/env python3
"""joyride for a player who has no build tools: the game's generated code
(lift.py's gen/, made from their own executable) compiled and linked to
joyride's runtime, which comes prebuilt (the AppImage, the Windows bundle).

  link_game.py <the runtime's folder> [--cc <C compiler>] [-o <out>]

The runtime's folder has link.json, written when it was built (by
tools/appimage.sh, tools/windows-bundle.sh), saying how its system links:

  { "exe": "joyride" | "joyride.exe",
    "cflags": [...]       the generated code's own, beside these always:
                          -O1 -w -DRECOMP_MEM_BASE=..., its headers
    "libs": [...]         in link order: files relative to the folder,
                          or -l names
    "ldflags": [...] }      ({rt} in a flag or file: the folder)

The compiler: --cc, else CC, else the folder's cc/joyride-cc(.cmd), else
gcc. Output: build/joyride(.exe) in joyride's source folder.
"""
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def main(argv):
    args = list(argv[1:])
    opt = {}
    for flag in ('--cc', '-o'):
        if flag in args:
            i = args.index(flag)
            opt[flag] = args[i + 1]
            del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    rt = os.path.abspath(args[0])
    with open(os.path.join(rt, 'link.json')) as f:
        how = json.load(f)
    cc = opt.get('--cc') or os.environ.get('CC')
    for c in ('cc/joyride-cc', 'cc/joyride-cc.cmd', 'cc/bin/gcc.exe'):
        if not cc and os.path.isfile(os.path.join(rt, c)):
            cc = os.path.join(rt, c)
    cc = cc or shutil.which('gcc')
    if not cc:
        sys.exit('no C compiler')

    gen = os.path.join(ROOT, 'gen', 'wino', 'recomp')
    srcs = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith('.c')) if os.path.isdir(gen) else []
    if not srcs:
        sys.exit(f'no generated code in {gen}: run tools/lift.py first')
    objdir = os.path.join(ROOT, 'build', 'gen')
    os.makedirs(objdir, exist_ok=True)
    out = opt.get('-o') or os.path.join(ROOT, 'build', how.get('exe', 'joyride'))
    pcr = os.path.join(ROOT, 'external', 'pcrecomp')

    # the lifted game: -O1, quietly, as CMakeLists.txt's joyride_gen
    base = [cc, '-O1', '-w', '-DRECOMP_MEM_BASE=0x100000000000ull',
            '-I', os.path.join(pcr, 'runtime', 'recomp32'), '-I', gen,
            '-include', os.path.join(gen, 'recomp_funcs.h'),
            '-include', os.path.join(ROOT, 'src', 'hle', 'texfilter_inline.h')] + how.get('cflags', [])
    print(f'compiling the game ({len(srcs)} files)'); sys.stdout.flush()

    def one(src):
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(src))[0] + '.o')
        r = subprocess.run(base + ['-c', src, '-o', obj], capture_output=True, text=True)
        return src, obj, r.returncode, r.stderr
    objs = []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for src, obj, code, err in ex.map(one, srcs):
            if code:
                sys.stdout.write(err[-3000:])
                sys.exit(f'cannot compile {os.path.basename(src)}')
            print(f'  {os.path.basename(src)}'); sys.stdout.flush()
            objs.append(obj)

    # joyride's runtime and what it links to, as its system does
    sub = lambda v: v.replace('{rt}', rt)                 # the folder, where a flag wants a path
    libs = [sub(l) if l.startswith('-') else os.path.join(rt, sub(l)) for l in how.get('libs', [])]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    print(f'linking {out}'); sys.stdout.flush()
    r = subprocess.run([cc, '-o', out] + objs + libs + [sub(f) for f in how.get('ldflags', [])])
    if r.returncode:
        sys.exit('the link failed')
    print(f'joyride ready: {out}')


if __name__ == '__main__':
    main(sys.argv)
