#!/usr/bin/env python3
"""joyride: GTA's executable translated to C with pcrecomp, on Linux or
Windows. Everything it writes is derived from the game binary and lands in
the git-ignored gen/ directory (never shared: made on each player's machine).

  lift.py <path/to/Grand Theft Auto.exe> [--cc <C compiler>]

Then every generated file is parsed as the build compiles it, with the C
compiler given (or CC, or gcc), so a lift that would not build stops here.
Runs with the Python it is started with (the AppImage's or the Windows
bundle's own, or the checkout's .venv), which has the tools' packages
(tools/requirements.txt). tools/lift.sh is the same, for a shell.
"""
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
WANT_SHA1 = 'd36976d66599cd649bf24f8ccd45bda84c0ee3f8'   # the 2002 release (Steam's, Rockstar Classics')


def run(args, out_file=None, cwd=None):
    """a step, its output to a file (or through); stops at a failure"""
    with open(out_file, 'w') if out_file else open(os.devnull, 'w') as f:
        r = subprocess.run(args, cwd=cwd, stdout=f if out_file else None,
                           stderr=subprocess.STDOUT if out_file else None)
    if r.returncode:
        if out_file:
            with open(out_file, errors='replace') as f:
                sys.stdout.write(''.join(f.readlines()[-20:]))
        sys.exit(f'failed: {" ".join(os.path.basename(a) if i == 0 else a for i, a in enumerate(args))}')


def parse_check(cc, gen, jobs):
    pcr = os.path.join(ROOT, 'external', 'pcrecomp')
    files = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith('.c'))
    env = dict(os.environ, LC_ALL='C')

    def one(path):
        r = subprocess.run([cc, '-w', '-fsyntax-only', '-DRECOMP_MEM_BASE=0x100000000000ull',
                            '-I', os.path.join(pcr, 'runtime', 'recomp32'), '-I', os.path.join(ROOT, 'src', 'hle'),
                            '-include', os.path.join(gen, 'recomp_funcs.h'),
                            '-include', os.path.join(ROOT, 'src', 'hle', 'texfilter_inline.h'), path],
                           env=env, capture_output=True, text=True)
        return path, r.returncode, r.stderr
    bad = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for path, code, err in ex.map(one, files):
            if code:
                bad.append(path)
                sys.stdout.write(err[-2000:])
    if bad:
        sys.exit(f'{len(bad)} generated files do not parse')
    print('all generated files parse')


def main(argv):
    args = list(argv[1:])
    cc = os.environ.get('CC') or shutil.which('gcc')
    if '--cc' in args:
        i = args.index('--cc')
        cc = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    if cc and (os.sep in cc or '/' in cc):
        cc = os.path.abspath(cc)                         # Windows starts a program by a full path only
    exe = os.path.abspath(args[0])
    if not os.path.isfile(exe):
        sys.exit(f'no {exe}')
    py = sys.executable
    out = os.path.join(ROOT, 'gen', 'wino')
    pcr = os.path.join(ROOT, 'external', 'pcrecomp')
    if not os.path.isfile(os.path.join(pcr, 'tools', '__main__.py')):
        sys.exit('pcrecomp is missing: git submodule update --init')

    with open(exe, 'rb') as f:
        got = hashlib.sha1(f.read()).hexdigest()
    if got != WANT_SHA1:
        print(f'warning: {exe} is not the 2002 release (sha1 {got})')

    os.makedirs(out, exist_ok=True)
    print('unpacking'); sys.stdout.flush()
    run([py, os.path.join(ROOT, 'tools', 'unpack.py'), exe, os.path.join(out, 'mine')], os.path.join(out, 'unpack.txt'))
    print('finding the functions'); sys.stdout.flush()
    run([py, os.path.join(ROOT, 'tools', 'funcs.py'), os.path.join(out, 'mine')], os.path.join(out, 'funcs.txt'))
    print('translating (a minute or two)'); sys.stdout.flush()
    run([py, '-m', 'tools', exe, '--all', '-o', os.path.join(out, 'recomp')], os.path.join(out, 'lift.log'), cwd=pcr)
    with open(os.path.join(out, 'lift.log'), errors='replace') as f:
        sys.stdout.write(''.join(f.readlines()[-4:]))
    run([py, os.path.join(ROOT, 'tools', 'postlift.py'), os.path.join(out, 'recomp')])
    if cc:
        print('checking the generated files parse'); sys.stdout.flush()
        parse_check(cc, os.path.join(out, 'recomp'), os.cpu_count() or 4)
    else:
        print('no C compiler: the parse check skipped')


if __name__ == '__main__':
    main(sys.argv)
