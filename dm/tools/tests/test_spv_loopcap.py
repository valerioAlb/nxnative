"""dm/android/spv_loopcap.h (every loop of a translated shader ended after a cap of trips, so one that never ends cannot hold the GPU until Windows resets it),
built for the PC with the host cc and run on modules assembled with spirv-as; spirv-val checks each result. Skipped without cc, spirv-as or spirv-val on PATH
(or the NDK's shader-tools). With DM_SPV_DIR=<a game's packed spv folder>, also run over every module there."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]

# a single-block loop whose back edge is on the false side, then a loop with no exit but the cap (OpBranch to its own header), then a do-while
FS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %o
OpExecutionMode %main OriginUpperLeft
OpDecorate %o Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%po = OpTypePointer Output %v4
%o = OpVariable %po Output
%int = OpTypeInt 32 1
%bool = OpTypeBool
%i0 = OpConstant %int 0
%i1 = OpConstant %int 1
%i9 = OpConstant %int 9
%main = OpFunction %void None %fn
%e = OpLabel
OpBranch %h
%h = OpLabel
%x = OpPhi %int %i0 %e %y %h
%y = OpIAdd %int %x %i1
%c = OpSGreaterThan %bool %y %i9
OpLoopMerge %m %h None
OpBranchConditional %c %m %h
%m = OpLabel
OpBranch %h2
%h2 = OpLabel
OpLoopMerge %m2 %h2 None
OpBranch %h2
%m2 = OpLabel
OpBranch %h3
%h3 = OpLabel
OpLoopMerge %m3 %c3 None
OpBranch %b3
%b3 = OpLabel
OpBranch %c3
%c3 = OpLabel
OpBranchConditional %c %h3 %m3
%m3 = OpLabel
OpReturn
OpFunctionEnd
'''

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "spv_loopcap.h"
int main(int argc, char **argv) {
    if (argc < 4) return 9;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 8; fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint32_t *w = malloc(sz); if (fread(w, 1, sz, f) != (size_t)sz) return 7; fclose(f);
    size_t n = sz / 4, need = 0; unsigned capped = 0, skipped = 0;
    uint32_t *s = malloc(4 * (size_t)w[3]); static spv_lc_loop L[SPV_LOOPCAP_MAX];
    spv_loopcap(w, n, (uint32_t)atoi(argv[3]), s, L, NULL, 0, &need, &capped, &skipped);
    if (!need) { printf("0 %u\n", skipped); return 1; }
    uint32_t *o = malloc(need * 4); size_t k = spv_loopcap(w, n, (uint32_t)atoi(argv[3]), s, L, o, need, &need, &capped, &skipped);
    if (!k) return 2;
    f = fopen(argv[2], "wb"); fwrite(o, 4, k, f); fclose(f); printf("%u %u\n", capped, skipped); return 0;
}
'''


def tool(name):
    return shutil.which(name)


@unittest.skipUnless(tool('cc') and tool('spirv-as') and tool('spirv-val'), 'needs cc, spirv-as and spirv-val')
class SpvLoopCap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'h.c').write_text(HARNESS)
        subprocess.run(['cc', '-O1', '-Wall', '-I', str(REPO / 'dm' / 'android'), str(cls.tmp / 'h.c'), '-o', str(cls.tmp / 'h')], check=True)

    def run_on(self, spv, cap=4096):
        out = self.tmp / 'o.spv'
        r = subprocess.run([str(self.tmp / 'h'), str(spv), str(out), str(cap)], capture_output=True, text=True)
        return r.returncode, r.stdout.split(), out

    def test_three_loops(self):
        src = self.tmp / 'fs.spvasm'; src.write_text(FS); spv = self.tmp / 'fs.spv'
        subprocess.run(['spirv-as', str(src), '-o', str(spv)], check=True)
        rc, out, o = self.run_on(spv)
        self.assertEqual(rc, 0); self.assertEqual(out, ['3', '0'])
        subprocess.run(['spirv-val', str(o)], check=True)
        dis = subprocess.run(['spirv-dis', str(o)], capture_output=True, text=True).stdout if tool('spirv-dis') else ''
        if dis: self.assertEqual(dis.count('OpULessThan'), 3)

    def test_game_dir(self):
        d = os.environ.get('DM_SPV_DIR')
        if not d: self.skipTest('DM_SPV_DIR not set')
        for p in sorted(Path(d).glob('*.spv')):
            rc, _, o = self.run_on(p)
            if rc == 0: subprocess.run(['spirv-val', str(o)], check=True)


if __name__ == '__main__':
    unittest.main()
