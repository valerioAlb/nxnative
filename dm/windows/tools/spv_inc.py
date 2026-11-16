# A SPIR-V module → the C initializer glslc -mfmt=c writes ({0x07230203, ...}), which dm/android/gfx_vk.c includes for the replay's own shaders.
# For builds with glslang (a release download, or glslangValidator) and no glslc. Usage: spv_inc.py <in.spv> <out.inc>
import struct, sys
data = open(sys.argv[1], 'rb').read()
assert len(data) % 4 == 0 and data[:4] == b'\x03\x02\x23\x07', 'not a SPIR-V module'
words = struct.unpack(f'<{len(data) // 4}I', data)
lines = [', '.join(f'0x{w:08x}' for w in words[i:i + 8]) for i in range(0, len(words), 8)]
res = '{' + ',\n'.join(lines) + '}\n'
try:
    if open(sys.argv[2]).read() == res: sys.exit(0)
except FileNotFoundError: pass
open(sys.argv[2], 'w', newline='\n').write(res)
