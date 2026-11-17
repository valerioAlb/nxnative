"""dm/android/spv_demote.h (DM_SPV_DEMOTE=1: a fragment shader's OpKill made OpDemoteToHelperInvocation and a return), built with the host cc and run on a module
assembled with spirv-as: a discard in the entry point under a branch (as the translator writes NVN's KIL, Pokémon Pokopia's UI shaders 4e7e5539bd1600d2 and
4045c5aea118d78b) and one in a function that returns an int. spirv-val checks the result. Skipped without cc, spirv-as or spirv-val on PATH."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]

FS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %o
OpExecutionMode %main OriginUpperLeft
OpDecorate %o Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%fni = OpTypeFunction %int
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%po = OpTypePointer Output %v4
%o = OpVariable %po Output
%bool = OpTypeBool
%t = OpConstantTrue %bool
%i1 = OpConstant %int 1
%f0 = OpConstant %float 0
%z = OpConstantComposite %v4 %f0 %f0 %f0 %f0
%main = OpFunction %void None %fn
%e = OpLabel
OpSelectionMerge %m None
OpBranchConditional %t %k %m
%k = OpLabel
OpKill
%m = OpLabel
%r = OpFunctionCall %int %h
OpStore %o %z
OpReturn
OpFunctionEnd
%h = OpFunction %int None %fni
%he = OpLabel
OpSelectionMerge %hm None
OpBranchConditional %t %hk %hm
%hk = OpLabel
OpKill
%hm = OpLabel
OpReturnValue %i1
OpFunctionEnd
'''

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "spv_demote.h"
int main(int argc, char **argv) {
    if (argc < 3) return 9;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 8; fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint32_t *w = malloc(sz); if (fread(w, 1, sz, f) != (size_t)sz) return 7; fclose(f);
    size_t n = sz / 4, need = 0; unsigned k = 0;
    spv_demote(w, n, NULL, 0, &need, &k); if (!need) return 1;
    uint32_t *o = malloc(need * 4); size_t r = spv_demote(w, n, o, need, &need, &k); if (!r) return 2;
    f = fopen(argv[2], "wb"); fwrite(o, 4, r, f); fclose(f); printf("%u\n", k); return 0;
}
'''


@unittest.skipUnless(shutil.which('cc') and shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs cc, spirv-as and spirv-val')
class SpvDemote(unittest.TestCase):
    def test_kills(self):
        t = Path(tempfile.mkdtemp())
        (t / 'h.c').write_text(HARNESS); (t / 'fs.spvasm').write_text(FS)
        subprocess.run(['cc', '-O1', '-Wall', '-I', str(REPO / 'dm' / 'android'), str(t / 'h.c'), '-o', str(t / 'h')], check=True)
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', str(t / 'fs.spvasm'), '-o', str(t / 'fs.spv')], check=True)
        r = subprocess.run([str(t / 'h'), str(t / 'fs.spv'), str(t / 'o.spv')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0); self.assertEqual(r.stdout.strip(), '2')
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(t / 'o.spv')], check=True)


if __name__ == '__main__':
    unittest.main()
