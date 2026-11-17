# scripts/build_windows_from_nsp.py: put_tree (the game folder kept in step with the build's outputs, unchanged files left alone) and --only (a rebuild from the last build's state)
import json, os, shutil, sys, tempfile, unittest
from typing import Any
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
bw: Any = None


def setUpModule():   # imported when its tests run, not when collected: build_android_from_nsp.py, imported first, would replace what other test files load their own copy to patch
    global bw
    sys.path.insert(0, str(REPO / 'scripts'))
    try: import build_windows_from_nsp as m   # pyright: ignore[reportMissingImports]
    except ImportError as e: raise unittest.SkipTest(f'build_windows_from_nsp.py does not import here: {e}') from e
    bw = m


class PutTree(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True)
        self.src, self.dst = self.tmp / 'src', self.tmp / 'dst'
        (self.src / 'sub').mkdir(parents=True)
        for n in ('a.spv', 'b.spv', 'sub/c.txt'): (self.src / n).write_text(n)

    def files(self, d): return sorted(str(p.relative_to(d)) for p in d.rglob('*') if p.is_file())

    def test_copies_then_keeps_unchanged(self):
        self.assertEqual(bw.put_tree(self.src, self.dst), 3); self.assertEqual(self.files(self.dst), self.files(self.src))
        self.assertEqual(bw.put_tree(self.src, self.dst), 0)   # nothing changed: nothing written
        self.assertEqual(bw.put_tree(self.src, self.dst, link=False), 0)

    def test_changed_new_and_removed_files(self):
        bw.put_tree(self.src, self.dst, link=False)
        (self.src / 'a.spv').write_text('changed, longer'); (self.src / 'd.spv').write_text('d'); (self.src / 'b.spv').unlink(); shutil.rmtree(self.src / 'sub')
        (self.dst / 'stray').write_text('x')
        self.assertEqual(bw.put_tree(self.src, self.dst, link=False), 2)
        self.assertEqual(self.files(self.dst), ['a.spv', 'd.spv']); self.assertEqual((self.dst / 'a.spv').read_text(), 'changed, longer')

    def test_a_file_where_a_folder_goes(self):
        self.dst.mkdir(); (self.dst / 'sub').write_text('was a file')
        bw.put_tree(self.src, self.dst); self.assertEqual((self.dst / 'sub/c.txt').read_text(), 'sub/c.txt')

    def test_hard_links(self):
        bw.put_tree(self.src, self.dst)
        self.assertTrue(os.path.samefile(self.src / 'a.spv', self.dst / 'a.spv'))


class Only(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_no_state_says_run_a_full_build(self):
        with mock.patch.object(bw, 'llvm_mingw', return_value=self.tmp), self.assertRaisesRegex(bw.BuildError, 'run a full build once'):
            bw.main(['--only', 'host', '--out', str(self.tmp / 'nothing')])

    def test_host_rebuilds_from_the_state(self):
        out = self.tmp / '0400A3A01EE4C000_windows'; out.mkdir(); work = self.tmp / 'work'
        (out / bw.STATE).write_text(json.dumps(dict(id='0400A3A01EE4C000', dir=str(self.tmp), game='g', patched='p', spv=None, font='f', aoc='a', defaults='',
                                                    work=str(work), env={'DM_TARGET': 'windows'}, meta={}, runtime=False)))
        exe = self.tmp / 'dmhost.exe'; exe.write_text('x'); st = self.tmp / 'st'; st.mkdir()
        seen = {}
        def host(self_, gid, w, logs, env): seen.update(gid=gid, work=w, target=env.get('DM_TARGET')); return (exe,)
        with mock.patch.object(bw, 'llvm_mingw', return_value=self.tmp), mock.patch.object(bw.Windows, 'host', host), \
             mock.patch.object(bw.Windows, 'selftest', return_value=st), mock.patch.object(bw.Windows, 'finish', side_effect=AssertionError('staged the game')):
            bw.main(['--only', 'host', str(self.tmp)])   # found as <NSP folder>/<id>_windows
        self.assertEqual(seen, dict(gid='0400A3A01EE4C000', work=work, target='windows')); self.assertTrue((out / 'dmhost.exe').is_file())


    def test_host_from_an_older_folder_with_only_game_json(self):   # staged before build_state.json existed
        out = self.tmp / 'pokopia'; (out / 'game').mkdir(parents=True); (out / 'game/game.json').write_text(json.dumps({'title_id': '0400a3a01ee4c000'}))
        exe = self.tmp / 'dmhost.exe'; exe.write_text('x'); st = self.tmp / 'st'; st.mkdir(); seen = {}
        def host(self_, gid, w, logs, env): seen['gid'] = gid; return (exe,)
        with mock.patch.object(bw, 'llvm_mingw', return_value=self.tmp), mock.patch.object(bw.Windows, 'host', host), mock.patch.object(bw.Windows, 'selftest', return_value=st), \
             mock.patch.object(bw.ba, 'data_dir', return_value=self.tmp / 'data' / 'android'):
            bw.main(['--only', 'host', '--out', str(out)])
        self.assertEqual(seen['gid'], '0400A3A01EE4C000'); self.assertTrue((out / 'dmhost.exe').is_file())


if __name__ == '__main__':
    unittest.main()
