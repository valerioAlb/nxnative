"""dm/android/spv_cbbound.h (uniform buffer reads at a computed index give 0 past the declared array, as Maxwell's constant buffers), built with the host cc and run
on a vertex shader assembled with spirv-as in the translator's shape: c[1] as 4096 vec4 read at a row computed from a vertex attribute (Pokémon Pokopia's world shader
85c01ed67ec089e9 reads it so, and hung the Adreno X1-45 on a row gigabytes past the buffer), as a vec4 and as a float; a read at a constant row is left as it is.
spirv-val checks the result. Skipped without cc, spirv-as or spirv-val on PATH."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]

VS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %a %pos
OpDecorate %a Location 1
OpDecorate %pos BuiltIn Position
OpDecorate %arr ArrayStride 16
OpDecorate %blk Block
OpMemberDecorate %blk 0 Offset 0
OpDecorate %c1 DescriptorSet 0
OpDecorate %c1 Binding 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%n = OpConstant %uint 4096
%arr = OpTypeArray %v4 %n
%blk = OpTypeStruct %arr
%pblk = OpTypePointer Uniform %blk
%c1 = OpVariable %pblk Uniform
%pv4 = OpTypePointer Uniform %v4
%pf = OpTypePointer Uniform %float
%pin = OpTypePointer Input %v4
%a = OpVariable %pin Input
%pout = OpTypePointer Output %v4
%pos = OpVariable %pout Output
%i0 = OpConstant %int 0
%i2 = OpConstant %int 2
%i16 = OpConstant %int 16
%main = OpFunction %void None %fn
%e = OpLabel
%av = OpLoad %v4 %a
%z = OpCompositeExtract %float %av 2
%t = OpExtInst %float %glsl Trunc %z
%r = OpConvertFToS %int %t
%row = OpIMul %int %r %i16
%p = OpAccessChain %pv4 %c1 %i0 %row
%v = OpLoad %v4 %p
%q = OpAccessChain %pf %c1 %i0 %row %i2
%s = OpLoad %float %q
%k = OpAccessChain %pv4 %c1 %i0 %i2
%kv = OpLoad %v4 %k
%m = OpVectorTimesScalar %v4 %v %s
%o = OpFAdd %v4 %m %kv
OpStore %pos %o
OpReturn
OpFunctionEnd
'''.replace('OpMemoryModel', '%glsl = OpExtInstImport "GLSL.std.450"\nOpMemoryModel', 1)

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "spv_cbbound.h"
int main(int argc, char **argv) {
    if (argc < 3) return 9;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 8; fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint32_t *w = malloc(sz); if (fread(w, 1, sz, f) != (size_t)sz) return 7; fclose(f);
    size_t n = sz / 4, need = 0; unsigned c = 0, l = 0; uint32_t *s = malloc(3 * 4 * (size_t)w[3]);
    spv_cbbound(w, n, s, NULL, 0, &need, &c, &l); if (!need) return 1;
    uint32_t *o = malloc(need * 4); size_t r = spv_cbbound(w, n, s, o, need, &need, &c, &l); if (!r) return 2;
    f = fopen(argv[2], "wb"); fwrite(o, 4, r, f); fclose(f); printf("%u %u\n", c, l); return 0;
}
'''


@unittest.skipUnless(shutil.which('cc') and shutil.which('spirv-as') and shutil.which('spirv-val') and shutil.which('spirv-dis'), 'needs cc and SPIRV-Tools')
class SpvCbBound(unittest.TestCase):
    def test_computed_rows(self):
        t = Path(tempfile.mkdtemp())
        (t / 'h.c').write_text(HARNESS); (t / 'vs.spvasm').write_text(VS)
        subprocess.run(['cc', '-O1', '-Wall', '-I', str(REPO / 'dm' / 'android'), str(t / 'h.c'), '-o', str(t / 'h')], check=True)
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', str(t / 'vs.spvasm'), '-o', str(t / 'vs.spv')], check=True)
        r = subprocess.run([str(t / 'h'), str(t / 'vs.spv'), str(t / 'o.spv')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr); self.assertEqual(r.stdout.split(), ['2', '2'])   # the two computed rows, not the constant one
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(t / 'o.spv')], check=True)
        d = subprocess.run(['spirv-dis', str(t / 'o.spv')], capture_output=True, text=True, check=True).stdout
        self.assertEqual(d.count('OpULessThan'), 2); self.assertEqual(d.count('OpConstantNull'), 3)   # the index's type, vec4 and float
        self.assertIn('OpTypeBool', d); self.assertIn('OpCompositeConstruct %v4bool', d)


if __name__ == '__main__':
    unittest.main()
