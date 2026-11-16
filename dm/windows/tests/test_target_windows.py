# The rewriter's Windows on Arm target (DM_TARGET=windows, dm/tools/nso_asm.py) on the synthetic module of dm/android/tests/selftest_nso.py, without game files:
#   - the module's layout says "# TARGET windows" (what loader.c checks on Windows)
#   - no mrs / msr of tpidr_el0 / tpidrro_el0 and no cntpct_el0 read is left in the module's own code (all rewritten to the slots / the gate)
#   - the slot base is read as mov xt, x18 (x18 is the TEB on Windows), as on Android; Apple's rewrite keeps mrs xt, tpidrro_el0
#   - the Windows and Android rewrites are the same code (the two differ only in the host's slot layout behind x18)
# Needs a clang that assembles for aarch64 (llvm-mingw's, the NDK's or any LLVM) and its llvm-objcopy: CLANG / OBJCOPY, else LLVM_MINGW/bin, else PATH.
# DM_SELFTEST_OUT=<folder>: also leave the Windows-rewritten module there (<folder>/main.* and the layout), for dmhost_cli.exe <folder> main on a Windows arm64 machine.
# Usage: python3 dm/windows/tests/test_target_windows.py [-v]
import os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def tool(env, name):
    if os.environ.get(env): return os.environ[env]
    if os.environ.get('LLVM_MINGW'):
        p = Path(os.environ['LLVM_MINGW']) / 'bin' / name
        if p.exists() or p.with_suffix('.exe').exists(): return str(p)
    p = shutil.which(name)
    if not p: raise unittest.SkipTest(f'no {name}: set {env} or LLVM_MINGW')
    return p


def rewrite(nso, out, target):
    env = dict(os.environ, DM_TARGET=target)
    r = subprocess.run([sys.executable, str(REPO / 'dm/tools/patch_nso.py'), str(nso), str(out), 'main'], env=env, capture_output=True, text=True)
    if r.returncode: raise AssertionError(f'patch_nso.py DM_TARGET={target} failed:\n{r.stdout}\n{r.stderr}')
    layout = dict(l[2:].split('\t', 1) for l in (out / 'main.fixups.tsv').read_text().splitlines() if l.startswith('# ') and '\t' in l)
    return (out / 'main.patched.bin').read_bytes(), layout


class TargetWindows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try: import capstone   # patch_nso.py needs it too
        except ImportError: raise unittest.SkipTest('capstone is missing (pip install "capstone>=5,<6")')
        cls.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); cls.cs.skipdata = True
        cls.tmp = Path(tempfile.mkdtemp(prefix='dm_win_rw_'))
        nso = cls.tmp / 'main.nso'
        subprocess.check_call([sys.executable, str(REPO / 'dm/android/tests/selftest_nso.py'), tool('CLANG', 'clang'), tool('OBJCOPY', 'llvm-objcopy'), str(nso)])
        cls.out = {t: rewrite(nso, cls.tmp / t, t) for t in ('windows', 'android', 'apple')}
        if os.environ.get('DM_SELFTEST_OUT'):
            d = Path(os.environ['DM_SELFTEST_OUT']); d.mkdir(parents=True, exist_ok=True)
            for f in (cls.tmp / 'windows').iterdir(): shutil.copy2(f, d / f.name)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def insns(self, target, lo=0, hi=None):
        b, layout = self.out[target]
        return [(i.address, f'{i.mnemonic} {i.op_str}') for i in self.cs.disasm(b[lo:hi], lo) if i.mnemonic != '.byte']

    def test_layout_target(self):
        self.assertEqual(self.out['windows'][1].get('TARGET'), 'windows')
        self.assertEqual(self.out['android'][1].get('TARGET'), 'android')

    def test_no_system_registers_left(self):
        b, layout = self.out['windows']
        end = int(layout['TEXT_END'], 16)
        left = [(hex(a), t) for a, t in self.insns('windows', 0, end) if 'tpidr' in t or 'cntpct' in t or (t.startswith('svc') and a > 0)]
        self.assertEqual(left, [], 'system register reads / svcs left in the rewritten code')

    def test_slot_base_is_x18(self):
        b, layout = self.out['windows']
        stubs = self.insns('windows', int(layout['PATCH_OFF'], 16))
        self.assertTrue(any(t.startswith('mov ') and t.endswith(', x18') for a, t in stubs), 'no mov xt, x18 slot base in the patch area')
        self.assertFalse(any('tpidrro_el0' in t for a, t in stubs), 'mrs tpidrro_el0 in the Windows patch area')
        apple = self.insns('apple', int(self.out['apple'][1]['PATCH_OFF'], 16))
        self.assertTrue(any('tpidrro_el0' in t for a, t in apple), 'the Apple rewrite should keep mrs xt, tpidrro_el0')

    def test_same_code_as_android(self):
        self.assertEqual(self.out['windows'][0], self.out['android'][0])


if __name__ == '__main__':
    unittest.main()
