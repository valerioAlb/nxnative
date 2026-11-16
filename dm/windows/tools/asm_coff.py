# Mach-O assembly → COFF assembly (aarch64-w64-mingw32), for the Windows build of the host's .S files (entry.S, nvn_entry.S, nvn_thunks.S), which are written for
# Apple's assembler. The rules of dm/android/tools/asm_elf.py, for COFF:
#   sym@PAGEOFF → :lo12:sym, sym@PAGE → sym (adrp takes the page of a plain symbol)
#   _name → name (Mach-O prefixes C symbols with an underscore; Windows arm64 does not), with asm_elf.py's renames for Darwin functions provided under another name
#   .private_extern → nothing (an .exe exports nothing; every symbol links within it)
#   .cstring → .section .rdata
#   .zerofill __DATA,__bss,sym,size,align → sym in .bss, 2^align aligned, then back to .text
# Usage: asm_coff.py <input .S> <output .S>
import os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'android', 'tools'))
from asm_elf import RENAME

def convert(text):
    out = []
    for line in text.splitlines():
        code, sep, comment = line.partition('//')
        code = re.sub(r'(?<![\w.:])_([A-Za-z]\w*)', lambda m: RENAME.get(m.group(1), m.group(1)), code)
        code = re.sub(r'\b(\w+)@PAGEOFF\b', r':lo12:\1', code)
        code = re.sub(r'\b(\w+)@PAGE\b', r'\1', code)
        code = re.sub(r'^\s*\.private_extern\b.*$', '', code)
        code = re.sub(r'^(\s*)\.cstring\b', r'\1.section .rdata,"dr"', code)
        code = re.sub(r'^(\s*)\.zerofill\s+__DATA\s*,\s*__bss\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*$',
                      r'\1.bss\n\1.p2align \4\n\2:\n\1.zero \3\n\1.text ', code)
        out.append(code + sep + comment)
    return '\n'.join(out) + '\n'

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, encoding='utf-8') as f: text = f.read()
    res = convert(text)
    try:
        with open(dst, encoding='utf-8') as f:
            if f.read() == res: sys.exit(0)   # unchanged: keep the file's time, so the build does not reassemble it
    except FileNotFoundError: pass
    with open(dst, 'w', encoding='utf-8', newline='\n') as f: f.write(res)
