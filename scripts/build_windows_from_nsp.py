#!/usr/bin/env python3
# Pack a Switch game you own for Windows on Arm (arm64: Snapdragon X, 8cx): your NSP / XCI files and prod.keys, a NUP, or a dump folder in; a folder you can start the game from out.
#   python scripts/build_windows_from_nsp.py <NSP folder | .nup | dump folder> [-k <prod.keys>] [-p <version>] [-l <language>] [--out <folder>] [--romfs copy|link|path]
#   python scripts/build_windows_from_nsp.py --only host|stage [--out <folder> | <NSP folder>]   after a full build: the host (and the game folder) again, nothing before it
#   python scripts/build_windows_from_nsp.py --host-only --title <title ID> [--out <folder>]    the host alone (dmhost.exe for that game's profile) and the self test, no game files
# The steps up to the shaders are build_android_from_nsp.py's (identify the game → extract → rewrite → split and translate the shaders to SPIR-V), with the rewrite for Windows
# (dm/tools/patch_nso.py with DM_TARGET=windows: the thread slots through x18, which is the TEB there) into <id>_build/patched_windows_<version>/. Then, in place of the Android
# host, the APK and the phone:
#   1. the game's host for Windows on Arm (dm/windows, CMake + llvm-mingw): dmhost.exe (with a window) and dmhost_cli.exe (none), in <id>_build/windows_host/
#   2. the self test (dm/android/tests/selftest_nso.py, rewritten with DM_TARGET=windows), which dmhost.exe runs when it finds no game
#   3. the folder (--out, default <NSP folder>/<id>_windows):
#        dmhost.exe  dmhost_cli.exe  env.txt (yours: KEY=VALUE lines, kept when there)  selftest/  README.txt
#        game/  code/ (the rewritten modules)  RomFS.img or RomFS/  spv/ (the shaders)  font/  aoc/ (DLC)  defaults.txt  game.json  icon.jpg  control.nacp
#      --romfs link (default): the RomFS image hard-linked from the extraction when it is on the same drive, else copied; copy: always copied; path: not copied, defaults.txt's
#      DM_ROMFS names the extraction's image (keep the build folder then)
# Start it with dmhost.exe in that folder (or dmhost.exe --console for a console with the log; log.txt is written beside it either way). Saves: %LOCALAPPDATA%\nxnative\<title ID>.
# Runs on the Surface itself (Windows 11 arm64: Python, the .NET SDK 10, CMake, Ninja, llvm-mingw's aarch64 release, glslang or the Vulkan SDK's glslc), or on Linux / macOS / x64 Windows
# with llvm-mingw's release for that system (then copy the folder over). See docs/WINDOWS_ARM.md.
# Nothing here downloads game files, keys or firmware: they are yours, read where you give them.
import argparse, json, os, re, shutil, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
import build_android_from_nsp as ba   # noqa: E402
import build_ipa_from_nsp as bi   # noqa: E402
from build_ipa_from_nsp import BuildError, say   # noqa: E402

EXE = '.exe' if os.name == 'nt' else ''
ENV_TXT = '''# nxnative for Windows on Arm: KEY=VALUE lines read before the game's defaults.txt (a key here wins). Examples (remove the # to use one):
# DM_OPMODE=0          handheld mode (720p): lighter on the GPU than docked 1080p (DM_OPMODE=1, the default here)
# DM_VSYNC=0           no vertical sync (mailbox / immediate presentation)
# DM_FULLSCREEN=1      start full screen (F11 or Alt+Enter switches)
# DM_RUMBLE=0          no controller rumble
# DM_PAD_ABXY=label    the controller's A/B/X/Y by label (an Xbox pad's A is the Switch's B by position, the default)
# DM_WIN_LAZY=2        commit the guest's memory on first touch for ranges of 64 MB and more (1: all, 0: none)
# DM_VK_DRIVER=<path>  another Vulkan loader or ICD-bearing vulkan-1.dll than System32's
# DM_DBG=1             more log lines (the guest's debug output among them)
'''


def tool(names, given=None, hint=''):
    for n in ([given] if given else []) + list(names):
        p = shutil.which(n) if n and not Path(n).is_file() else n
        if p: return str(p)
    raise BuildError(f'none of {", ".join(names)} found{": " + hint if hint else ""}')


def llvm_mingw(arg):   # --llvm-mingw, LLVM_MINGW, else the folder of an aarch64-w64-mingw32-clang on PATH
    for c in (arg, os.environ.get('LLVM_MINGW')):
        if c and (Path(c) / 'bin' / f'aarch64-w64-mingw32-clang{EXE}').exists(): return Path(c)
    p = shutil.which('aarch64-w64-mingw32-clang')
    if p: return Path(p).resolve().parent.parent
    raise BuildError('llvm-mingw not found: unpack a release from https://github.com/mstorsjo/llvm-mingw/releases (on the Surface: llvm-mingw-<date>-ucrt-aarch64.zip) '
                     'and give --llvm-mingw <folder> or set LLVM_MINGW')


def link_or_copy(src, dst):
    try: os.link(src, dst)
    except OSError: shutil.copy2(src, dst)
    return dst


def same(a, b):   # b is a's hard link, or a copy with its size and modification time (copy2 keeps it)
    return a.st_ino == b.st_ino and a.st_dev == b.st_dev and a.st_ino or (a.st_size == b.st_size and a.st_mtime_ns == b.st_mtime_ns)

def put_tree(src, dst, link=True):
    """dst made the same as src: its files hard-linked where the drive allows (the shaders: >100k files, gigabytes), else copied; a file already there unchanged is kept,
    one src no longer has removed. A rebuild after a host change stages the game in seconds, not by deleting and linking every shader again. → files written"""
    if dst.is_symlink() or dst.is_file(): dst.unlink()
    dst.mkdir(parents=True, exist_ok=True)
    put = 0; have = {e.name: e for e in os.scandir(dst)}
    for e in os.scandir(src):
        d = have.pop(e.name, None); t = dst / e.name
        if e.is_dir():
            if d is not None and not d.is_dir(follow_symlinks=False): os.unlink(t)
            put += put_tree(Path(e.path), t, link)
            continue
        if d is not None:
            if d.is_dir(follow_symlinks=False): shutil.rmtree(t)
            elif same(e.stat(), d.stat(follow_symlinks=False)): continue
            else: os.unlink(t)
        (link_or_copy if link else shutil.copy2)(e.path, t); put += 1
    for d in have.values():
        if d.is_dir(follow_symlinks=False): shutil.rmtree(d.path)
        else: os.unlink(d.path)
    return put


class Windows:
    def __init__(self, a):
        self.a = a; self.lm = llvm_mingw(a.llvm_mingw)

    def host(self, gid, work, logs, env):   # → (dmhost.exe, dmhost_cli.exe) for game profile gid; Ninja rebuilds only what changed (DM_TUNE / DM_CPU: dm/windows/CMakeLists.txt)
        cmake = tool(['cmake'], self.a.cmake, 'install CMake 3.22 or newer (pip install cmake)')
        ninja = tool(['ninja'], self.a.ninja, 'install Ninja (pip install ninja)')
        bdir = work / 'windows_host'
        cache = bdir / 'CMakeCache.txt'   # a cache made from another checkout: cmake refuses it (build_android_from_nsp.py build_host)
        if cache.is_file():
            m = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache.read_text(encoding='utf-8', errors='replace'), re.M)
            if m and Path(m.group(1).strip()).resolve() != (REPO / 'dm/windows').resolve(): shutil.rmtree(bdir)
        cfg = [cmake, '-G', 'Ninja', '-S', REPO / 'dm/windows', '-B', bdir, f'-DCMAKE_MAKE_PROGRAM={ninja}', f'-DLLVM_MINGW={self.lm}',
               f'-DCMAKE_TOOLCHAIN_FILE={REPO / "dm/windows/cmake/aarch64-w64-mingw32.cmake"}', f'-DGAME={gid}', f'-DPython3_EXECUTABLE={sys.executable}']
        if self.a.glslang: cfg.append(f'-DGLSLANG={self.a.glslang}')
        for k in ('DM_TUNE', 'DM_CPU'):   # the compiler's tuning (default oryon-1, Snapdragon X) and an optional -mcpu, from the environment
            if os.environ.get(k) is not None: cfg.append(f'-D{k}={os.environ[k]}')
        bi.run(cfg, logs / 'windows_cmake.log', env=env)
        bi.run([ninja, '-C', bdir] + (['-j', str(self.a.jobs)] if self.a.jobs else []) + ['dmhost_app', 'dmhost_cli'], logs / 'windows_host.log', env=env)
        return bdir / 'dmhost.exe', bdir / 'dmhost_cli.exe'

    def selftest(self, work, logs, env):   # the synthetic module, rewritten for Windows → its folder
        d = work / 'windows_selftest'; d.mkdir(parents=True, exist_ok=True)
        b = self.lm / 'bin'
        bi.run([sys.executable, REPO / 'dm/android/tests/selftest_nso.py', b / f'clang{EXE}', b / f'llvm-objcopy{EXE}', d / 'main.nso'], logs / 'windows_selftest.log', env=env)
        bi.run([sys.executable, REPO / 'dm/tools/patch_nso.py', d / 'main.nso', d / 'patched', 'main'], logs / 'windows_selftest_rewrite.log', env=dict(env, DM_TARGET='windows'))
        return d / 'patched'

    def stage_host(self, out, exes, st):
        out.mkdir(parents=True, exist_ok=True)
        for e in exes: shutil.copy2(e, out / e.name)
        put_tree(st, out / 'selftest', link=False)
        if not (out / 'env.txt').exists(): (out / 'env.txt').write_text(ENV_TXT, encoding='utf-8')

    def finish(self, g, game, patched, spv, font, aoc, defaults, work, logs, env, meta, runtime):
        out = Path(self.a.out).expanduser().resolve() if self.a.out else work.parent / f'{g.id}_windows'
        state = dict(id=g.id, dir=str(g.dir), game=str(game), patched=str(patched), spv=str(spv) if spv else None, font=str(font), aoc=str(aoc), defaults=defaults, work=str(work),
                     env={k: v for k, v in env.items() if os.environ.get(k) != v}, meta=meta, runtime=bool(runtime))
        out.mkdir(parents=True, exist_ok=True)
        (out / STATE).write_text(json.dumps(state, indent=1), encoding='utf-8')   # for --only host / stage: the next rebuild skips the steps before them
        bi.PROG.stage('Windows host'); exes = self.host(g.id, work, logs, env); st = self.selftest(work, logs, env); bi.PROG.end(str(exes[0]))
        bi.PROG.stage('game folder')
        self.stage_host(out, exes, st)
        gd = out / 'game'; gd.mkdir(exist_ok=True)
        put_tree(patched, gd / 'code', link=False)
        put_tree(font, gd / 'font', link=False)
        if spv: put_tree(spv, gd / 'spv')
        elif (gd / 'spv').exists(): shutil.rmtree(gd / 'spv')
        if aoc.is_dir() and any(aoc.iterdir()): put_tree(aoc, gd / 'aoc')
        for f in ('icon.jpg', 'control.nacp'):
            if (game / f).is_file(): shutil.copy2(game / f, gd / f)
        rom = Path(str(bi.rfs.source(game))); lines = defaults.splitlines()
        for old in ('RomFS', 'RomFS.img'):
            p = gd / old
            if p.is_dir(): shutil.rmtree(p)
            elif p.exists(): p.unlink()
        if rom.is_dir(): put_tree(rom, gd / 'RomFS')   # a RomFS folder from an older extraction
        elif self.a.romfs == 'path': lines.append(f'DM_ROMFS={rom}')
        elif self.a.romfs == 'copy': say(f'copying the RomFS image ({rom.stat().st_size / 1e9:.1f} GB)'); shutil.copy2(rom, gd / 'RomFS.img')
        else: link_or_copy(rom, gd / 'RomFS.img')
        own = g.dir / 'windows_defaults.txt'   # a profile's Windows-only defaults, after its Android ones (defaults_text): a later key wins
        if own.is_file(): lines += [ln.strip() for ln in own.read_text(encoding='utf-8').splitlines() if '=' in ln and not ln.lstrip().startswith('#')]
        (gd / 'defaults.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
        (gd / 'game.json').write_text(json.dumps(meta, indent=1), encoding='utf-8', newline='\n')
        (out / 'README.txt').write_text(readme(meta, out), encoding='utf-8')
        bi.PROG.end(str(out))
        say(f'done: {out}. On the Windows on Arm PC: copy the folder there (if built elsewhere) and start dmhost.exe (dmhost.exe --console shows the log; log.txt is beside it)')
        if runtime: say('the game compiles shaders while it runs: its captures land in game/glslc_capture; build again to translate them')


STATE = 'build_state.json'   # in the output folder: what the last full build staged from (Windows.finish), for --only


def only(w, a):
    """--only host: the host and the self test built again (Ninja: the changed sources only) and put in the folder, the game left as it is.
    --only stage: that, and the game folder staged again from the last build's outputs. Neither unpacks, rewrites or reads the shaders"""
    cands = [Path(a.out).expanduser().resolve()] if a.out else []
    if a.nsp_dir:
        src = Path(a.nsp_dir).expanduser().resolve()
        for b in (src, src.parent):
            cands += sorted(b.glob(f'{a.title_id.upper() if a.title_id else "*"}_windows'))
    st = next((c / STATE for c in cands if (c / STATE).is_file()), None)
    if st is None: raise BuildError(f'--only {a.only}: no {STATE} from a full build in {", ".join(map(str, cands)) or "(give --out <the game folder> or the NSP folder / .nup)"}: run a full build once')
    s = json.loads(st.read_text(encoding='utf-8')); out = st.parent; work = Path(s['work']); logs = work / 'logs'; logs.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, **s['env'])
    if a.only == 'host':
        bi.PROG.stage('Windows host'); exes = w.host(s['id'], work, logs, env); stt = w.selftest(work, logs, env); bi.PROG.end(str(exes[0]))
        w.stage_host(out, exes, stt); say(f'done: {out} (host rebuilt; game/ unchanged)'); return
    w.a.out = str(out)
    g = type('G', (), dict(id=s['id'], dir=Path(s['dir'])))()
    w.finish(g, Path(s['game']), Path(s['patched']), Path(s['spv']) if s['spv'] else None, Path(s['font']), Path(s['aoc']), s['defaults'], work, logs, env, s['meta'], s['runtime'])


def readme(meta, out):
    return f'''{meta.get("name", "")} ({meta.get("title_id", "")}) {meta.get("version", "")} for Windows on Arm (arm64), packed by nxnative's scripts/build_windows_from_nsp.py.
Start: dmhost.exe (a window; F11 or Alt+Enter: full screen). dmhost.exe --console: with a console showing the log. dmhost_cli.exe <folder> main: no window.
Keyboard: WASD left stick, IJKL right stick, arrows D-pad, Z/X/C/V = A/B/X/Y, E/U = L/R, Q/O = ZL/ZR, F/H = stick clicks, Enter = +, Backspace = -. XInput pads 1-4; the mouse touches.
Settings: env.txt (KEY=VALUE lines, a key there wins over game/defaults.txt). Log: log.txt beside dmhost.exe. Saves and caches: %LOCALAPPDATA%\\nxnative\\{meta.get("title_id", "<title ID>")}.
No game files are in this folder unless you put them there: keep it to yourself.
'''


def main(argv):
    ap = argparse.ArgumentParser(description='Pack a Switch game you own for Windows on Arm (dmhost.exe and the game folder)')
    ap.add_argument('nsp_dir', nargs='?'); ap.add_argument('-k', '--keys', help='your prod.keys (not needed for a NUP or a dump folder)')
    ap.add_argument('-p', '--package', help='the version to pack (default: the newest in the folder)')
    ap.add_argument('-l', '--language', default='en-US', help='the game language (default English)')
    ap.add_argument('--title-id', help='the game in a folder that holds several (16 hex digits)')
    ap.add_argument('--out', help='the folder to write (default <NSP folder>/<title ID>_windows)')
    ap.add_argument('--romfs', choices=('link', 'copy', 'path'), default='link', help='the RomFS image: hard-linked (else copied), copied, or named by path (default link)')
    ap.add_argument('--llvm-mingw', help='the llvm-mingw folder (default LLVM_MINGW, else PATH)')
    ap.add_argument('--glslang', help='glslang / glslangValidator (default: glslc or glslang on PATH)')
    ap.add_argument('--cmake'); ap.add_argument('--ninja')
    ap.add_argument('--host-only', action='store_true', help='build the host and the self test only (no game files), with --title')
    ap.add_argument('--title', help='with --host-only: the game profile (dm/games/<title ID>)')
    ap.add_argument('--allow-damaged', action='store_true', help="as build_android_from_nsp.py's")
    ap.add_argument('-j', '--jobs', type=int, help='shader translator processes and Ninja jobs (default: one per core, fewer when the free memory holds fewer translators)')
    ap.add_argument('--skip-shaders', action='store_true', help='reuse the shaders a build translated before (<id>_build/spv) without checking them against the package')
    ap.add_argument('--only', choices=('host', 'stage'), help='host: rebuild dmhost.exe and put it in the folder of the last full build; stage: that and the game folder again')
    a = ap.parse_args(argv)
    if a.jobs: os.environ['DM_SPV_JOBS'] = str(a.jobs)
    if a.skip_shaders: os.environ['DM_SKIP_SPV'] = '1'
    w = Windows(a)
    if a.only: return only(w, a)
    if a.host_only:
        if not a.title or not re.fullmatch(r'[0-9A-Fa-f]{16}', a.title): raise BuildError('--host-only needs --title <16-digit title ID with a profile in dm/games>')
        gid = a.title.upper(); base = ba.data_dir().parent / 'windows' / gid   # %LOCALAPPDATA%\nxnative\windows\<id> on Windows (beside the Android library)
        out = Path(a.out).expanduser().resolve() if a.out else base / 'package'
        work = base / 'work'; logs = work / 'logs'; logs.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, PYTHONUTF8='1')
        exes = w.host(gid, work, logs, env); st = w.selftest(work, logs, env); w.stage_host(out, exes, st)
        say(f'done: {out} (dmhost.exe with no game/ runs the self test; put a game folder from a full build in game/)'); return
    if not a.nsp_dir: raise BuildError('give the NSP folder, the .nup file or the dump folder (or --host-only --title <id>)')
    ba.WINDOWS = w
    args = [a.nsp_dir, '--no-push', '-l', a.language]
    if a.keys: args += ['-k', a.keys]
    if a.package: args += ['-p', a.package]
    if a.title_id: args += ['--title-id', a.title_id]
    if a.allow_damaged: args += ['--allow-damaged']
    ba.main(args)


if __name__ == '__main__':
    try: main(sys.argv[1:])
    except BuildError as e:
        bi.PROG.close(ok=False); bi.PROG.say(f'[RED] {e}'); sys.exit(1)
    finally:
        bi.PROG.close(ok=False)
