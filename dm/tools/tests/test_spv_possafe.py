"""dm/android/spv_possafe.h (a vertex shader's position: NaN or infinite made (0,0,0,1), every component clamped to +-1e15, before each return), built with the
host cc and run on modules assembled with spirv-as; spirv-val checks each result. Skipped without cc, spirv-as or spirv-val on PATH.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every module there."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]

HEAD = '''OpCapability Shader
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
'''
# the position in a gl_PerVertex block, as the translator writes it (Pokopia's 85c01ed67ec089e9), with an early return
VS_BLOCK = HEAD + '''OpEntryPoint Vertex %main "main" %pv %vid
OpMemberDecorate %blk 0 BuiltIn Position
OpMemberDecorate %blk 1 BuiltIn PointSize
OpDecorate %blk Block
OpDecorate %vid BuiltIn VertexIndex
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%int = OpTypeInt 32 1
%bool = OpTypeBool
%blk = OpTypeStruct %v4 %float
%pblk = OpTypePointer Output %blk
%pv = OpVariable %pblk Output
%pin = OpTypePointer Input %int
%vid = OpVariable %pin Input
%pov4 = OpTypePointer Output %v4
%i0 = OpConstant %int 0
%one = OpConstant %float 1
%vec = OpConstantComposite %v4 %one %one %one %one
%main = OpFunction %void None %fn
%l = OpLabel
%p = OpAccessChain %pov4 %pv %i0
OpStore %p %vec
%i = OpLoad %int %vid
%c = OpIEqual %bool %i %i0
OpSelectionMerge %m None
OpBranchConditional %c %early %m
%early = OpLabel
OpReturn
%m = OpLabel
OpReturn
OpFunctionEnd
'''
# the position as a variable, no bool type declared
VS_VAR = HEAD + '''OpEntryPoint Vertex %main "main" %pos
OpDecorate %pos BuiltIn Position
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%pv4 = OpTypePointer Output %v4
%pos = OpVariable %pv4 Output
%one = OpConstant %float 1
%vec = OpConstantComposite %v4 %one %one %one %one
%main = OpFunction %void None %fn
%l = OpLabel
OpStore %pos %vec
OpReturn
OpFunctionEnd
'''
FS = HEAD + '''OpEntryPoint Fragment %main "main" %c0
OpExecutionMode %main OriginUpperLeft
OpDecorate %c0 Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%po = OpTypePointer Output %v4
%c0 = OpVariable %po Output
%one = OpConstant %float 1
%vec = OpConstantComposite %v4 %one %one %one %one
%main = OpFunction %void None %fn
%l = OpLabel
OpStore %c0 %vec
OpReturn
OpFunctionEnd
'''

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "spv_possafe.h"
int main(int argc, char **argv) {
    if (argc < 3) return 9;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 8; fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint32_t *w = malloc(sz); if (fread(w, 1, sz, f) != (size_t)sz) return 7; fclose(f);
    size_t n = sz / 4; if (!spv_possafe_wanted(w, n)) { printf("unwanted\n"); return 1; }
    size_t cap = n + SPV_POSSAFE_DECL + SPV_POSSAFE_PER * 64; uint32_t *o = malloc(cap * 4); unsigned nr = 0;
    size_t k = spv_possafe(w, n, o, cap, &nr);
    if (!k) { printf("0\n"); return 2; }
    f = fopen(argv[2], "wb"); fwrite(o, 4, k, f); fclose(f); printf("%u\n", nr); return 0;
}
'''


def tool(name):
    return shutil.which(name)


@unittest.skipUnless(tool('cc') and tool('spirv-as') and tool('spirv-val'), 'needs cc, spirv-as and spirv-val')
class SpvPosSafe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'h.c').write_text(HARNESS)
        subprocess.run(['cc', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm' / 'android'), str(cls.tmp / 'h.c'), '-o', str(cls.tmp / 'h')], check=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def asm(self, text, name):
        src = self.tmp / f'{name}.spvasm'; src.write_text(text); spv = self.tmp / f'{name}.spv'
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', str(src), '-o', str(spv)], check=True)
        return spv

    def run_on(self, spv):
        out = self.tmp / (spv.stem + '.out.spv')
        r = subprocess.run([str(self.tmp / 'h'), str(spv), str(out)], capture_output=True, text=True)
        return r.returncode, r.stdout.split(), out

    def check(self, text, name, returns):
        rc, out, o = self.run_on(self.asm(text, name))
        self.assertEqual(rc, 0, out); self.assertEqual(out, [str(returns)])
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(o)], check=True)
        if tool('spirv-dis'):
            dis = subprocess.run(['spirv-dis', str(o)], capture_output=True, text=True).stdout
            self.assertEqual(dis.count('OpIsNan'), returns); self.assertEqual(dis.count('FClamp'), returns)

    def test_block_member_each_return(self): self.check(VS_BLOCK, 'blk', 2)

    def test_variable_and_new_bool_types(self): self.check(VS_VAR, 'var', 1)

    def test_fragment_shader_left_alone(self):
        rc, out, _ = self.run_on(self.asm(FS, 'fs')); self.assertEqual(rc, 1); self.assertEqual(out, ['unwanted'])

    def test_game_dir(self):
        d = os.environ.get('DM_SPV_DIR')
        if not d: self.skipTest('DM_SPV_DIR not set')
        for p in sorted(Path(d).glob('*.spv')):
            rc, _, o = self.run_on(p)
            if rc == 0: subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(o)], check=True)


if __name__ == '__main__':
    unittest.main()
