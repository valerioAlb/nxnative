# A game's shaders translated for the Android Vulkan replay (dm/android/gfx_vk_draw.inc), from the package dm/gfx/tools/shader_split.py writes: what dmsh_build.py
# does for Metal, without the Metal steps. Each record of <package dir>/<stage>_<group>.bin ('BTCDBTCD', a header with +0x28 the control section's length and +0x2c
# the code area's, padded to 0x38, the code area, the control section) becomes a dmsh_xlate job as dmsh_build.py job_for makes it (the 0x30 header stripped, a
# compute area's instructions from +0x100, c1 from control +0x700..+0x704, the profile's translation facts), with softfloat64 (phones' GPUs have no FP64). The
# translator's output goes through spv_android.py: <out dir>/<key>.spv, <key>.z.spv (vertex) and <key>.res.json, which the app finds in game/spv (DM_SPV_DIR).
# The key is the host's (hle_nvn.c keys_hash): sha256 of the control section (its own length when +0x20 gives one under 0x880) and the code area, 16 hex digits.
# NVN2 (Switch 2) records are Ampere code, as dmsh_build.py job_for takes them (dm/gfx/tools/dmsh_ampere.py): the first 0x48 control bytes with +0x30 as zero in the key,
# the instructions (the 0x80-byte SPH included) as the job's code, c1 between control +0x40 and +0x44, job arch sm86, and compute's c0 layout (nvn2_cs). Task and mesh
# shaders are translated as compute with their stage memory in shared memory, as dmsh_build.py job_for does, and spv_android.py makes them Vulkan task and mesh modules
# (spv_mesh.py; the replay draws them on VK_EXT_mesh_shader, gfx_vk_mesh.inc).
# Vertex shaders in two parts (VertexA + VertexB, Catherine: Full Body's GLSLC) are merged into one program first, as dmsh_build.py vertex_ab does.
# The code areas, the job lists and the translator's output go in a temporary folder under WORK (build_android_from_nsp.py sets it to <id>_build/tmp), which the run
# removes however it ends: for a big game it holds tens of GB, so it stays on the game's drive, beside its other files, and a failed run leaves nothing behind.
# A run that had shaders to translate and translated none of them stops (none_made): the build must not pack the game without them.
# Batches (--batch, DM_SPV_BATCH): the shaders go through the translator, its integer-input pass and the post-pass a batch at a time, each batch's results landing in
# <out dir> before the next starts. A run that stops (closed, killed, a full disk) keeps every finished batch, and the next run takes only the shaders not there yet;
# the temporary folder holds one batch's code areas and output instead of the whole game's (Pokémon Pokopia's: tens of GB). The code areas are written as their batch
# starts, by several threads. -j translator processes at once (default: every core), each given a share of the batch in one job list (one process start per share).
# Usage: spv_build.py <package dir> <out dir> --xlate <dmsh_xlate.dll or executable> [--facts <shader_facts.tsv>] [-j N] [--batch N]
import spv_android as sa   # beside this file
import argparse, concurrent.futures as cf, contextlib, errno, hashlib, io, json, multiprocessing, os, re, shutil, struct, subprocess, sys, tempfile, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'gfx/tools')); import dmsh_ampere, dmsh_meshstage   # noqa: E402  NVN2 control sections and task / mesh facts, as dmsh_build.py reads them
GFX_MAGIC, CS_MAGIC, CTL_MAGIC = bytes.fromhex('78563412'), bytes.fromhex('79563412'), bytes.fromhex('34127698')

def meshemu_v():   # spv_meshemu.VERSION, the compute form's (a res.json mesh_emu of null, a shader it could not be made for, counts as this version)
    import spv_meshemu
    return spv_meshemu.VERSION


def spans(d):   # the records' (start, end) in a package file
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [(x, r[k + 1] if k + 1 < len(r) else len(d)) for k, x in enumerate(r)]

def records(d):
    return [d[x:y] for x, y in spans(d)]

def split(rec):   # → (code area, control section)
    nctl, ncode = struct.unpack_from('<2I', rec, 0x28)
    return rec[0x38:0x38 + ncode], rec[0x38 + ncode:0x38 + ncode + nctl]

def key_of(ctl, code):
    if dmsh_ampere.is_nvn2(ctl): return hashlib.sha256(ctl[:0x30] + bytes(4) + ctl[0x34:0x48] + code).hexdigest()[:16]   # shader_split.py key_ctl, the host's keys_hash
    own = struct.unpack_from('<I', ctl, 0x20)[0] if len(ctl) >= 0x24 else 0
    n = own if 0x760 <= own < 0x880 else 0x880
    return hashlib.sha256(ctl[:n] + code).hexdigest()[:16]

# dmsh_build.py tf_layout, the same (the capture is a .tf.spv variant here: spv_android.py tf_variant)
def tf_layout(key, ctl):
    """Transform feedback layout from a vertex shader's control section (what the NVN driver programs into the GPU's feedback state when the program is bound): +0x6dc u16 stride per buffer
    (4 buffers), +0x6e8 u8 per buffer the number of captured outputs, +0x6ec u8 buffer count and +0x6ed 1; per buffer b, +0xdc + 0x80 b lists the captured outputs, one byte each as 0x20 + n
    (generic output n), and +0x4dc + 0x80 b their component counts, one byte each, each captured from x on and packed in that order (stride = 4 x the components). Pokémon Legends: Z-A's
    28 skinning shaders capture one output per buffer (position xyz, packed normal / tangent bits); Donkey Kong Country: Tropical Freeze's fur simulation captures two in one buffer
    (`7dcdaed991c74bfe`: outputs 0 and 1, xyz each, stride 24; count 2, component bytes 03 03).
    → [[[[output, components], …], stride] per buffer, [] for an unused one], None for a shader without transform feedback. Any other form gets a red line and no capture entry point (the host
    then reports the draw, nvn_mtl.m tf_draw): it is not known how it maps, and one shader must not stop a game's build (Tears of the Kingdom and Ace Attorney have no feedback shader at all)"""
    strides = struct.unpack_from('<4H', ctl, 0x6dc)
    if not any(strides): return None
    out = []
    for b in range(4):
        if not strides[b]: out.append([]); continue
        k = ctl[0x6e8 + b]; locs = list(ctl[0xdc + 0x80 * b:0xdc + 0x80 * b + k]); comps = list(ctl[0x4dc + 0x80 * b:0x4dc + 0x80 * b + k])
        if (not 1 <= k <= 4 or any(ctl[0xdc + 0x80 * b + k:0x15c + 0x80 * b]) or any(ctl[0x4dc + 0x80 * b + k:0x4e0 + 0x80 * b])
                or not all(0x20 <= x < 0x40 for x in locs) or not all(1 <= n <= 4 for n in comps) or strides[b] != 4 * sum(comps)):
            print(f'[RED] {key}: transform feedback buffer {b} has a layout of unknown form (stride {strides[b]}, outputs {k}: {[hex(x) for x in locs]}, components {comps}): no capture entry point; see tf_layout', flush=True)
            return None
        out.append([[[x - 0x20, n] for x, n in zip(locs, comps)], strides[b]])
    return out


# dmsh_build.py vertex_ab, the same (that script runs when imported); keep the two alike
def vertex_ab(key, code, ctl):
    """A vertex shader in two parts, VertexA + VertexB, as one program (Catherine: Full Body's GLSLC, SDK 8.2.1, GPU code version 1.15, writes every vertex shader so; the other games' are one
    program of shader type 1). Part A sits at code +0, its SPH with shader type 0 (Maxwell's VERTEX_CULL_BEFORE_FETCH slot): it loads the position attribute and writes only the position.
    Part B starts at control +0x700 (its own 0x30-byte header, then an SPH of shader type 1) and writes the other outputs from the other attributes. The hardware runs A, then B; translated
    alone, A left every varying unwritten (a full-screen pass read its texture coordinate as 0 and the whole screen came out one texel's colour). Control +0x748 lists both parts as
    {SPH offset, length from the SPH, ?} for A, then for B. The merge is what Ryujinx's TranslatorContext.Translate(VertexA) builds: A's instructions, each unconditional EXIT a branch to
    B's first instruction, then B's, under B's SPH with A's flags and input / output maps ORed in and the larger local memory sizes. Measured on all 1,244 of the game's vertex shaders: A
    writes no generic output (so none of B's loads reads one of A's), A ends in two EXITs (one predicated on CC FCSM_TR, which Ryujinx never takes, left as it is) and a BRA to itself,
    and neither part has an absolute or table jump, so moving B keeps every branch. → (code area with its 0x30 header, c1), c1 being what follows part B; None for any other vertex shader"""
    if len(code) < 0x80 or len(ctl) < 0x760 or code[:4] != bytes.fromhex('78563412'): return None
    w0 = struct.unpack_from('<I', code, 0x30)[0]
    if w0 & 0x1f != 1 or (w0 >> 10) & 0xf != 0: return None
    a_sph, a_len, _, b_sph, b_len, _ = struct.unpack_from('<6I', ctl, 0x748)
    if not any((a_sph, a_len, b_sph, b_len)): return None   # one program of shader type 0, no part B: translated as it is (Ryujinx reads type 0 as a vertex shader)
    bw0 =struct.unpack_from('<I', code, b_sph)[0] if 0x80 <= b_sph <= len(code) - 0x50 else 0
    if a_sph != 0x30 or b_sph != struct.unpack_from('<I', ctl, 0x700)[0] + 0x30 or bw0 & 0x1f != 1 or (bw0 >> 10) & 0xf != 1 or (a_len - 0x50) % 32 or (b_len - 0x50) % 32 or b_sph + b_len > len(code):
        raise ValueError(f'vertex shader of shader type 0 without a VertexB part in the expected place (control +0x748 {a_sph:#x} {a_len:#x} {b_sph:#x} {b_len:#x})')
    sa, sb = list(struct.unpack_from('<20I', code, a_sph)), list(struct.unpack_from('<20I', code, b_sph))
    sph = [sb[0] | sa[0] & ~(0xf << 10)] + [max(sa[i] & 0xffffff, sb[i] & 0xffffff) | sb[i] & ~0xffffff for i in (1, 2, 3)] + [sb[4]] + [sa[i] | sb[i] for i in range(5, 20)]
    a = bytearray(code[a_sph + 0x50:a_sph + a_len]); b = code[b_sph + 0x50:b_sph + b_len]
    for o in range(0, len(a), 8):
        if o % 32 == 0: continue   # the scheduling word of each group of three instructions
        ins = struct.unpack_from('<Q', a, o)[0]
        if ins >> 52 == 0xe30 and ins & 0x1f == 0xf:   # EXIT with CC T (any predicate) → BRA to part B, keeping the predicate and CC bits; the offset counts from the next instruction
            struct.pack_into('<Q', a, o, 0xe24 << 52 | ((len(a) - (o + 8)) & 0xffffff) << 20 | ins & 0xfffff)
    return code[:0x30] + struct.pack('<20I', *sph) + bytes(a) + b, code[b_sph + b_len:]



def job(key, stage, code, ctl, w, facts):
    extra = {}
    if dmsh_ampere.is_nvn2(ctl):   # dmsh_build.py job_for's NVN2 branch: the Ampere instructions as they are (a graphics stage's 0x80-byte SPH included)
        c1 = dmsh_ampere.c1_segment(ctl, code); body = code[:dmsh_ampere.inst_len(ctl)]; extra['arch'] = 'sm86'
        if stage == 'cs': extra['nvn2_cs'] = [ctl[0xb1], ctl[0xb2], ctl[0xb3], struct.unpack_from('<H', ctl, 0xba)[0], ctl[0xa9] * 4, ctl[0xb4], ctl[0xb5]]   # compute's c0 layout
        if stage in ('ts', 'ms'):   # dmsh_build.py job_for: compute without the SPH, the stage memory in shared memory (dmsh_meshstage.memory); graphics' c[0] and c[2] tables stay
            mesh = dmsh_meshstage.info(ctl, code); lay, size = dmsh_meshstage.memory(mesh)
            if dmsh_meshstage.own_shared(body[0x80:]): return None, 'a task / mesh shader with shared memory of its own: it would lie over the stage memory'
            extra.update(mesh_stage=mesh, stage_memory=lay, local=[mesh['threads'], 1, 1], shared=size); body = body[0x80:]
    else:
        c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
        c1 = code[c1o:c1t] if c1o <= c1t <= len(code) else b''
        if stage == 'vs':
            try: ab = vertex_ab(key, code, ctl)
            except ValueError as e: return None, str(e)
            if ab: code, c1 = ab
        if stage != 'cs': body = code[0x30:]
        elif code[:4] == CS_MAGIC:
            if any(code[0x30:0x100]): return None, 'compute code area +0x30..+0x100 is not all zero'
            body = code[0x100:]
        else: body = code
    p = Path(w) / f'{key}.bin' if w is not None else None   # None: the job's fields only (translate's first look); its batch writes the code area
    if p is not None: p.write_bytes(body)
    j = {'key': key, 'stage': 'cs' if stage in ('ts', 'ms') else stage, 'code': str(p) if p is not None else None, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), 'softfloat64': True, **extra,
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # dmsh_build.py's options and defaults: the chain is tested with them
    if key in facts: j.update(facts[key])
    else: j['no_facts'] = True
    if stage == 'cs' and 'local' not in j and len(ctl) >= 0x758:   # the thread group and shared memory from the control section, as shader_split.py writes the facts
        x, y, z = struct.unpack_from('<3I', ctl, 0x748); sh = struct.unpack_from('<I', ctl, 0x754)[0]
        if x and y and z: j['local'] = [x, y, z]
        if sh and 'shared' not in j: j['shared'] = sh
    return j, None

def int_inputs_of(path):   # a pool job: one translated vertex shader's inputs that it only uses as integers, {location: type}, a few entries
    data = Path(path).read_bytes()
    if sa.unreadable(data): return {}   # empty or cut short (a write that failed): the post-pass gives it its [RED] line
    return sa.int_inputs(sa.parse(data)[1])

CHUNK = 16   # vertex shaders per pool job

def int_input_jobs(keys, jobs, tout, j):
    """The second pass for vertex shaders (dmsh_build.py's): inputs a translated shader only uses as integers are added to its job's attr as uint (spv_android.py int_inputs),
    and → those shaders' keys, in key order, to translate again. Reading each shader's SPIR-V is pure Python, and in one process it was most of the translation step: Super Mario
    Odyssey's 7,418 vertex shaders took 114 of its 151 s, Ocarina of Time's 15,851 about 118 s, while the translator had finished in 14 and 70. So j processes read them (spawn
    on every host, as Windows has only spawn), and each job returns only the few locations it found; the jobs are changed here, in key order, as one process changes them"""
    vs = [k for k in keys if jobs[k]['stage'] == 'vs' and (tout / f'{k}.spv').exists()]
    if sys.platform == 'win32': j = min(j, 61)   # ProcessPoolExecutor's limit on Windows (WaitForMultipleObjects); -j defaults to os.cpu_count()
    n = max(1, min(j, -(-len(vs) // CHUNK)))
    ex = cf.ProcessPoolExecutor(n, mp_context=multiprocessing.get_context('spawn')) if n > 1 else None
    paths, again, seen = [str(tout / f'{k}.spv') for k in vs], [], [0]
    if vs: step('checking vertex shaders')
    try:
        with counting(len(vs), lambda: seen[0]) if vs else contextlib.nullcontext():
            for k, found in zip(vs, ex.map(int_inputs_of, paths, chunksize=CHUNK) if ex else map(int_inputs_of, paths)):
                extra = {l: t for l, t in found.items() if l not in jobs[k]['attr']}
                if extra: jobs[k]['attr'].update(extra); again.append(k)
                seen[0] += 1
    except BaseException:
        if ex: ex.shutdown(cancel_futures=True)   # a shader that stops it stops it at once, not after every other job
        raise
    if ex: ex.shutdown()
    return again

# A full disk in a translator [RED] line (dmsh_xlate prints each shader's IOException and goes on to the next): POSIX's strerror, and Windows' ERROR_DISK_FULL and
# ERROR_HANDLE_DISK_FULL as .NET words them
DISK_FULL = re.compile(r'No space left on device|not enough space on the disk|The disk is full|\[Errno 28\]|ENOSPC', re.I)

def disk_full(path, why):
    """Stop on a full disk with a line build_android_from_nsp.py shows the player: what could not be written, the drive's free space, what to do. Going on reached the
    post-pass, which stopped at the first empty .spv the translator left (IndexError in spv_android.py parse, droidplay-core#307), and the log said nothing of the disk"""
    d = Path(os.path.abspath(path))
    while not d.is_dir() and d != d.parent: d = d.parent   # a file that could not be written may not be there
    try: free = f'{shutil.disk_usage(d).free / 1e9:.1f} GB free'
    except OSError: free = 'no free space it could read'
    print(f'[RED] spv_build: the disk is full: {why}. The drive with {d} has {free}, before this run\'s temporary files are removed. Nothing more is translated: '
          'free some space and build again (the shaders translated already are kept)', flush=True)
    raise SystemExit(1)

def stop_if_full(failed, tout, step):   # after a translation pass: the translator's failures, and a stop when any was a full disk
    full = sorted(k for k, why in failed.items() if DISK_FULL.search(why))
    if not full: return
    for k, why in sorted(failed.items()):
        if k not in full: print(f'[RED] {k}: {why}')   # its other failures, lost otherwise
    disk_full(tout, f"the translator could not write {len(full):,} shaders' output in its {step} ({full[0]}: {failed[full[0]]})")

def take(r, failed, said, other, prefix='', cut=160):
    """One translator process's output: its `[RED] <key>: <why>` lines into failed, and their keys into said (the shaders it gave a reason for); once, the first
    of its other lines with its exit code into other: what a translator that made nothing printed instead (dotnet's "Could not execute because the specified
    command or file was not found." for a dll that isn't there)"""
    reds = 0
    for ln in r.stderr.splitlines() + r.stdout.splitlines():
        m = re.match(r'\[RED\] (\w+): (.*)', ln)
        if m: failed[m.group(1)] = prefix + m.group(2)[:cut]; said.add(m.group(1)); reds += 1
        elif ln.strip() and not ln.startswith('translated: ') and not other: other.append(f'exit code {r.returncode}: {ln.strip()[:200]}')   # not its own summary line
    if r.returncode and not reds and not other: other.append(f'exit code {r.returncode}, nothing printed')

def none_made(keys, said, have, other, xlate):
    """Stop when this run had shaders to translate and translated none of them. A translator that didn't run or wrote nothing (its dll moved aside) gave every
    shader 'no output from the translator', and the run ended '0 translated, …, 30 failed' with exit code 0, so build_android_from_nsp.py packed the game without
    shaders (droidplay-core#307's follow-up). One such run goes on: every shader in it is one the translator said it can't handle, and earlier runs translated the
    game's others: Mario Kart World's 078997bf631992c5 ('Ampere instruction 481 at 0x0') fails on every build, and a later build reads '0 translated, 11576
    already there, 1 failed'. A run that translated some goes on, however many failed: no build has shown how many a game can lose and still play"""
    silent = [k for k in keys if k not in said]
    if not silent and have: return
    if silent:
        print(f'[RED] spv_build: no shader was translated: the translator made none of the {len(keys):,} shaders it was given, and gave a reason for '
              f'{f"only {len(keys) - len(silent):,}" if len(silent) < len(keys) else "none"} of them ({other[0] if other else "it printed nothing"}). Check that {xlate} '
              f'is there and runs{" with dotnet" if xlate.endswith(".dll") else ""}, then build again: a file missing from its folder does this', flush=True)
    else:
        print(f'[RED] spv_build: no shader was translated: the translator failed every one of the game\'s {len(keys):,} shaders (their [RED] lines above), and none is '
              'there from an earlier run, so the game would have none', flush=True)
    raise SystemExit(1)

# The room a run needs, per byte of the package records it translates (header, code area, control section), measured on the Mac from three games' finished
# <id>_build/spv folders against their shader/pkg, as allocated on disk. Their translated files: Ocarina of Time 12.1 bytes per package byte (42,848 shaders, 149 KiB
# each), Final Fantasy Resonance 13.9 (73,496, 125 KiB), Super Mario Odyssey 11.9 (27,984, 67 KiB). The translator's output, in the temporary folder until the run ends,
# is about their .spv and .res.json files: 0.80, 0.80 and 0.70 of that, so the peak is 21.8, 25.0 and 20.2. Per shader it varies far more than per byte: Pokémon Pokopia's
# records are 33 KiB each (4.4 GB for 131,426 shaders), 3 to 6 times theirs, and one of its runs on Windows had left a 45.7 GB temporary folder when the disk filled,
# 10.4 per byte for that half alone (OoT's 9.7 plus its code areas) though unfinished (droidplay-core#307). The lowest peak is taken, rounded down: 20, so the check
# stops a run that can't fit, as Pokopia's (88 GB and the spare) on the reporter's 70 GB, without stopping one that can
OUT_PER_BYTE, TEMP_PER_BYTE = 12, 8
ROOM_SPARE = 2 << 30   # left free besides: the log, the app, the copy to the phone's 256 MiB pieces

def same_drive(a, b):
    return os.stat(a).st_dev == os.stat(b).st_dev

def room(out, tmp, n, nbytes, reused, batch=0):
    """Stop before translating n shaders (nbytes of records) when the drives can't hold what the run makes: their translated files beside out, less reused (the files of an
    older chain they overwrite), and the translator's output in tmp until the run ends (the code areas are written already). Pokopia's disk filled about 12 minutes into
    the translation, and with nothing checked the run went on for 20 more (droidplay-core#307). DM_SPV_SPACE_CHECK=0 skips it"""
    if not n or os.environ.get('DM_SPV_SPACE_CHECK') == '0': return
    need_out, need_tmp = max(0, nbytes * OUT_PER_BYTE - reused), nbytes * TEMP_PER_BYTE * (min(1.0, batch / n) if batch else 1)   # the temporary folder holds one batch at a time
    gb = lambda b: f'{b / 1e9:.1f} GB'
    parts = {'out': f'{gb(need_out)} for the translated shaders', 'tmp': f"{gb(need_tmp)} for the translator's temporary copy of {'one batch of them' if batch and batch < n else 'them until the end'}"}
    for d, need, what in ([(out, need_out + need_tmp, f"{parts['out']} and {parts['tmp']}")] if same_drive(out, tmp) else
                          [(out, need_out, parts['out']), (tmp, need_tmp, parts['tmp'])]):
        free = shutil.disk_usage(d).free
        if free >= need + ROOM_SPARE: continue
        print(f'[RED] spv_build: not enough free space to translate {n:,} shaders ({nbytes / 1e6:,.0f} MB in the package): the drive with {d} has {gb(free)} free, and they need '
              f'about {gb(need + ROOM_SPARE)} there: {what}, and {gb(ROOM_SPARE)} to spare. Free {gb(need + ROOM_SPARE - free)} more there, or move the game\'s folder to a drive '
              'with more room, then build again. Nothing was translated (DM_SPV_SPACE_CHECK=0 translates anyway)', flush=True)
        raise SystemExit(1)

def work_dir():
    """Where the run's temporary folder goes: WORK, which build_android_from_nsp.py sets to <id>_build/tmp, else the system's temp folder (a run by hand). On Windows
    that one is on C: whatever drive the game is on, and out of sight: droidplay-core#307's three failed Pokémon Pokopia runs each left tens of GB in %TEMP%.
    Under WORK, a spv_* folder is one that a killed run left (a closed window runs no finally), so it is removed first"""
    d = os.environ.get('WORK')
    if not d: return None
    os.makedirs(d, exist_ok=True)
    for old in Path(d).glob('spv_*'): shutil.rmtree(old, ignore_errors=True)
    return d

READ_COUNT_MIN = 256 << 20   # a package this large gets a counted step for its first read (MB)
PROGRESS_SECS = 10   # how often the first pass says how far it got: build_android_from_nsp.py shows the count, and a game with a very large shader set (Pokopia) translates for hours


def translated(tout):   # the shaders the translator has finished so far in this run's output folder: each one's .spv, its reader variant aside (as `done` counts them)
    try: return sum(1 for e in os.scandir(tout) if e.name.endswith('.spv') and not e.name.endswith('.reader.spv'))
    except OSError: return 0


def progress(tout, total, stop, count=None, unit=''):   # 'progress: <done>/<total>' every PROGRESS_SECS while the first pass runs, when the count moved (droidplay-core#307: hours with nothing to show)
    last = -1
    while not stop.wait(PROGRESS_SECS):
        n = count() if count else translated(tout)
        if n != last: print(f'progress: {n}/{total}{unit}', flush=True); last = n


def step(name):   # 'step: <name>', which build_android_from_nsp.py shows as the step of the translation stage, its count starting again
    print(f'step: {name}', flush=True)


@contextlib.contextmanager
def counting(total, count, unit=''):
    """'progress: <done>/<total>' at the start, from count() every PROGRESS_SECS while the block runs, and at its end: the steps after the first pass, which said nothing,
    so a build showed 100 % for the rest of the stage (droidplay-core#437: Pokémon Legends: Z-A's 28,603 shaders sat at 100 % with only the clock moving)"""
    u = f' {unit}' if unit else ''
    print(f'progress: 0/{total}{u}', flush=True)
    stop = threading.Event(); watch = threading.Thread(target=progress, args=(None, total, stop, count, u), daemon=True); watch.start()
    try: yield
    finally:
        stop.set(); watch.join()
    print(f'progress: {count()}/{total}{u}', flush=True)


def written(out, keys, since):   # the shaders whose res.json the post-pass has written in out since it started (res.json is written last; an older run's is older)
    n = 0
    for k in keys:
        try: n += os.stat(out / f'{k}.res.json').st_mtime >= since
        except OSError: pass
    return n


def free_ram():   # bytes of physical memory free now, None where it can't be read
    try:
        if sys.platform == 'win32':
            import ctypes
            class MS(ctypes.Structure): _fields_ = [('len', ctypes.c_ulong), ('load', ctypes.c_ulong)] + [(n, ctypes.c_ulonglong) for n in ('tot', 'avail', 'totpf', 'availpf', 'totv', 'availv', 'ext')]
            m = MS(); m.len = ctypes.sizeof(MS)
            return m.avail if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else None
        try:   # Linux: what can be had without swapping, the page cache included
            with open('/proc/meminfo') as f:
                for line in f:
                    if line.startswith('MemAvailable:'): return int(line.split()[1]) << 10
        except OSError: pass
        return os.sysconf('SC_AVPHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
    except (OSError, ValueError, AttributeError): return None

MEM_PER_JOB = int(os.environ.get('DM_SPV_MEM_MB') or 700) << 20   # a translator process's peak on a large shader set's job list, with margin; DM_SPV_MEM_MB changes it
MEM_KEEP = 2 << 30   # left to the system and the post-pass

def ram_jobs(j):
    """-j limited by the memory free now (a 16 GB laptop runs as many translators as fit, not one per core and then pages): one MEM_PER_JOB each, MEM_KEEP left"""
    free = free_ram()
    if free is None or os.environ.get('DM_SPV_JOBS'): return j   # an explicit DM_SPV_JOBS / -j via it is taken as given
    fit = max(1, (free - MEM_KEEP) // MEM_PER_JOB)
    if fit < j: print(f'spv_build: {j} processes asked, {fit} fit in the {free / 2**30:.1f} GB of memory free ({MEM_PER_JOB >> 20} MB each): {fit}', flush=True)
    return int(min(j, fit))


STAMP = '.stamp'   # beside the output folder (<out>.stamp, not in it: the folder is copied to the game as it is): what the last complete run was made from (stamp_of) and what it left there

def stamp_of(pkgs, a):
    """The inputs a run depends on: the package files (names, sizes, modification times), the facts, the translator file, the chains of the post-pass and of the
    mesh shaders' compute form. A run with the same stamp and its outputs still there has nothing to do (a rebuild of the host skips minutes of reading 4 GB)"""
    h = hashlib.sha256()
    for f in pkgs:
        st = f.stat(); h.update(f'{f.name}\t{st.st_size}\t{st.st_mtime_ns}\n'.encode())
    for extra in (a.facts, a.xlate):
        if extra and os.path.exists(extra): st = os.stat(extra); h.update(f'{os.path.basename(extra)}\t{st.st_size}\t{st.st_mtime_ns}\n'.encode())
    h.update(f'chain {sa.ANDROID_CHAIN} meshemu {meshemu_v()}\n'.encode())
    return h.hexdigest()

def stamp_reuse(out, stamp):   # → the last run's summary line when its stamp matches and its outputs are all still there, else None
    try: d = json.loads(out.with_name(out.name + STAMP).read_text(encoding='utf-8'))
    except (OSError, ValueError): return None
    if d.get('stamp') != stamp or os.environ.get('DM_SPV_FORCE') == '1': return None
    try: n = sum(1 for e in os.scandir(out) if e.name.endswith('.res.json'))
    except OSError: return None
    return d if n >= d.get('made', 1 << 62) else None


def main():
    w = tempfile.mkdtemp(prefix='spv_', dir=work_dir())
    try: translate(w)
    except OSError as e:   # this script's own writes (the code areas, the job lists, reflections) on a full disk
        if e.errno != errno.ENOSPC: raise
        disk_full(e.filename or w, f'{e.filename or "a file"} could not be written')
    finally: shutil.rmtree(w, ignore_errors=True)   # the code areas and the translator's output: as large as the shaders made, whether the run finished or not

def translate(w):
    ap = argparse.ArgumentParser(description='translate a shader_split package for the Android Vulkan replay')
    ap.add_argument('package'); ap.add_argument('out'); ap.add_argument('--xlate', required=True); ap.add_argument('--facts'); ap.add_argument('-j', type=int, default=int(os.environ.get('DM_SPV_JOBS') or 0) or os.cpu_count() or 4)
    ap.add_argument('--batch', type=int, default=int(os.environ.get('DM_SPV_BATCH') or 0), help='shaders per batch (default: 250 per process, at least 2,000)')
    a = ap.parse_args()
    a.j = ram_jobs(max(1, a.j)); a.batch = a.batch if a.batch > 0 else max(2000, 250 * a.j)
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    pkgs = [f for f in sorted(Path(a.package).glob('*.bin')) if f.name[:2] in ('vs', 'ps', 'gs', 'tc', 'te', 'cs', 'ts', 'ms')]
    stamp = stamp_of(pkgs, a)
    if (d := stamp_reuse(out, stamp)):   # nothing changed since the last complete run: its summary, nothing read or translated (DM_SPV_FORCE=1 reads everything again)
        print(f"spv_build: 0 translated, {d['made']} already there (0 given their constant buffer extents), {d['failed']} failed, {d['skipped']} skipped, inputs unchanged since the last run")
        return
    try: out.with_name(out.name + STAMP).unlink()   # a run that stops leaves no stamp: the next one reads the package again
    except OSError: pass
    facts = {}
    if a.facts and os.path.exists(a.facts):
        for ln in open(a.facts, encoding='utf-8'):
            if ln.strip() and not ln.startswith('#'): k, v = ln.rstrip('\n').split('\t', 1); facts[k] = json.loads(v)
    jobs, skipped, have, tfs, upg, nbytes, reused, where = {}, {}, 0, {}, 0, 0, 0, {}
    total_b, seen_b = sum(f.stat().st_size for f in pkgs), [0]   # the first look at the package counted in MB (a game's 4.4 GB of records took minutes with nothing shown)
    big = total_b >= READ_COUNT_MIN   # a small package reads in a moment: no step of its own
    if big: step('reading the shader package')
    with counting(max(1, total_b >> 20), lambda: seen_b[0] >> 20, 'MB') if big else contextlib.nullcontext():
        for f in pkgs:
            stage = f.name[:2]; data = f.read_bytes()
            for x, y in spans(data):
                rec = data[x:y]; code, ctl = split(rec)
                if ctl[:4] != CTL_MAGIC: skipped[f'{f.name}?'] = 'no control section magic'; continue
                key = key_of(ctl, code)
                if key in jobs: continue
                rp = out / f'{key}.res.json'   # translated by an earlier run of this chain version (the key covers the shader's bytes)
                prev = (out / f'{key}.spv').exists() and rp.exists()
                if prev:
                    try: rj = json.loads(rp.read_text(encoding='utf-8'))
                    except ValueError: rj = {}   # empty or cut short, by a disk that filled while it was written: translated again
                    ch = rj.get('android')
                    if ch == sa.EXT_FROM and sa.ANDROID_CHAIN == sa.EXT_FROM + 1: sa.upgrade(out, key); ch = sa.EXT_FROM + 1; upg += 1   # (its .spv files are chain EXT_FROM + 1's: only res.json's extents were missing; after a later bump of ANDROID_CHAIN, translated again)
                    # a task or mesh shader made before spv_android.py wrote their compute form (res.json mesh_emu: its facts, or null when it could not be made) is translated
                    # again, alone: the other shaders keep their chain
                    if ch == sa.ANDROID_CHAIN and not (stage in ('ts', 'ms') and (rj.get('mesh_emu', {}) or {'v': 0}).get('v') != meshemu_v()): have += 1; continue
                j, why = job(key, stage, code, ctl, None, facts)
                if not j: skipped[key] = why; continue
                jobs[key] = j; nbytes += len(rec); where[key] = (f, x, y, stage)
                if prev: reused += sum(f.stat().st_size for f in (out / f'{key}.spv', out / f'{key}.z.spv', rp) if f.exists())   # an older chain's files, which this run overwrites
                if stage == 'vs' and not dmsh_ampere.is_nvn2(ctl) and (tf := tf_layout(key, ctl)): tfs[key] = tf
            seen_b[0] += len(data); del data
    room(out, Path(w), len(jobs), nbytes, reused, a.batch)
    keys = sorted(jobs)
    xl = [a.xlate] if not a.xlate.endswith('.dll') else ['dotnet', a.xlate]
    batches = [keys[i:i + a.batch] for i in range(0, len(keys), a.batch)]
    many = len(batches) > 1   # one batch: the steps and counts as before; several: one count for the whole run, each batch's steps in the log only
    failed, said, other, made_all, t0, base = {}, set(), [], 0, time.time(), [0]
    if many: print(f'spv_build: {len(keys):,} shaders in {len(batches)} batches of up to {a.batch:,}, {a.j} translator processes', flush=True); step('translating')
    # the shaders left to translate in this run (those an earlier run made are not counted), so a build that carries on after a stopped one starts from 0 of what is left
    if jobs: print(f'progress: 0/{len(jobs)}', flush=True)
    # Several batches: batch N's post-pass (spv_android.py, pure Python) runs in the background while batch N+1 translates, so the translator processes never wait
    # for it; at most one post-pass at a time, and a run that stops waits for the one running, whose batch then stays made
    pending, res = None, {}
    def post_bg(n, done, tout, bw):
        try: res[n] = post_batch(a, done, tout, out, failed, many, max(1, a.j // 2) if many else a.j)
        except BaseException as e: res[n] = e
        finally: shutil.rmtree(bw, ignore_errors=True)   # the batch's code areas and translator output: its results are in out now
    def settle():
        nonlocal pending, made_all
        if pending is None: return
        n, th = pending; th.join(); pending = None
        r = res.pop(n)
        if isinstance(r, BaseException): raise r
        made_all += r
    try:
        for bi_, bkeys in enumerate(batches):
            bw = Path(w) / f'b{bi_}'; bw.mkdir()
            done, tout = translate_batch(a, bkeys, jobs, where, facts, tfs, out, bw, xl, failed, said, other, many, base, len(jobs))
            settle()   # the previous batch's post-pass, before this one's starts
            if not many:
                try: made_all += post_batch(a, done, tout, out, failed, many, a.j)
                finally: shutil.rmtree(bw, ignore_errors=True)
            else:
                th = threading.Thread(target=post_bg, args=(bi_, done, tout, bw), daemon=True); th.start(); pending = (bi_, th)
            base[0] += len(bkeys)
            if many:
                dt = time.time() - t0; rate = base[0] / dt if dt > 0 else 0; left = (len(keys) - base[0]) / rate if rate else 0
                print(f'progress: {base[0]}/{len(jobs)}', flush=True)
                print(f'spv_build: batch {bi_ + 1}/{len(batches)} translated: {base[0]:,}/{len(keys):,} shaders in {dt / 60:.1f} min ({rate:.1f}/s), about {left / 60:.0f} min left', flush=True)
        settle()
    finally:
        if pending is not None: pending[1].join()   # stopped: the batch already translated is still written into out
    for k, why in sorted(skipped.items()): print(f'[RED] {k}: {why}')
    print(f'spv_build: {made_all} translated, {have} already there ({upg} given their constant buffer extents), {len(failed)} failed, {len(skipped)} skipped, {sum(1 for k in jobs if jobs[k].get("no_facts"))} without facts')
    if keys and not made_all: none_made(keys, said, have, other, a.xlate)
    out.with_name(out.name + STAMP).write_text(json.dumps({'stamp': stamp, 'made': made_all + have, 'failed': len(failed), 'skipped': len(skipped)}), encoding='utf-8')


def write_code_areas(keys, jobs, where, facts, bw, j):
    """The batch's code areas into bw (job writes each), in package order, by j threads: the files are written while the next records are read"""
    by_file = {}
    for k in keys: by_file.setdefault(where[k][0], []).append(k)
    with cf.ThreadPoolExecutor(max(1, min(j, 16))) as ex:
        futs = []
        for f, ks in by_file.items():
            with open(f, 'rb') as fh:
                for k in sorted(ks, key=lambda k: where[k][1]):
                    _, x, y, stage = where[k]; fh.seek(x); rec = fh.read(y - x); code, ctl = split(rec)
                    futs.append(ex.submit(job, k, stage, code, ctl, bw, facts))
        for k, fu in zip([k for f, ks in by_file.items() for k in sorted(ks, key=lambda k: where[k][1])], futs):
            jj, why = fu.result()
            if jj: jobs[k]['code'] = jj['code']   # (the same job as the first look made: the same bytes)


def translate_batch(a, keys, jobs, where, facts, tfs, out, w, xl, failed, said, other, many, base, total):
    """One batch: its code areas, the translator (a.j processes, a share of the batch each), the integer-input pass. → (the shaders translated, the folder they are in)"""
    write_code_areas(keys, jobs, where, facts, w, a.j)
    chunks = [keys[i::a.j] for i in range(a.j)]
    tout = Path(w) / 'out'; tout.mkdir()
    def run(ch):
        jp = Path(w) / f'job_{abs(hash(ch[0]))}.json'; jp.write_text(json.dumps([jobs[k] for k in ch]), encoding='utf-8')
        return subprocess.run(xl + [str(jp), str(tout)], capture_output=True, text=True, errors='replace')
    bfailed = {}
    stop = threading.Event()
    watch = threading.Thread(target=progress, args=(tout, total, stop, (lambda: base[0] + translated(tout)) if many else None), daemon=True)
    if keys: watch.start()
    try:
        with cf.ThreadPoolExecutor(a.j) as ex:
            for r in ex.map(run, [c for c in chunks if c]): take(r, bfailed, said, other)
    finally:
        stop.set()
        if watch.is_alive(): watch.join()
    if keys and not many: print(f'progress: {translated(tout)}/{total}', flush=True)
    stop_if_full(bfailed, tout, 'first pass')
    quiet = contextlib.redirect_stdout(io.StringIO()) if many else contextlib.nullcontext()   # several batches: no step lines per batch (they would restart the build's count)
    with quiet: again = int_input_jobs(keys, jobs, tout, a.j)
    if again:
        if not many: step('translating vertex shaders again')
        cs, redone = [c for c in [again[i::a.j] for i in range(a.j)] if c], [0]
        with cf.ThreadPoolExecutor(a.j) as ex, (counting(len(again), lambda: redone[0]) if not many else contextlib.nullcontext()):
            for c, r in zip(cs, ex.map(run, cs)): take(r, bfailed, said, other, 'integer-input retranslation: ', 130); redone[0] += len(c)
        print(f'spv_build: {len(again)} vertex shaders translated again with integer inputs')
        stop_if_full(bfailed, tout, 'integer-input pass')   # it rewrites good .spv files in place, so a disk that fills here cuts them short
    done = sorted(p.name[:-4] for p in tout.glob('*.spv') if not p.name.endswith('.reader.spv'))
    for k in done:   # the feedback layout and a task / mesh shader's facts go with the shader's reflection: spv_android.py makes the capture variant and the stage modules from them
        if k in tfs or 'mesh_stage' in jobs[k]:
            rp = tout / f'{k}.res.json'
            try: r = json.loads(rp.read_text(encoding='utf-8'))
            except (FileNotFoundError, ValueError): continue   # none, or one cut short: the post-pass gives the shader its [RED] line
            if k in tfs: r['tf'] = tfs[k]
            if 'mesh_stage' in jobs[k]: r['mesh'] = {'info': jobs[k]['mesh_stage'], 'lay': jobs[k]['stage_memory']}
            rp.write_text(json.dumps(r), encoding='utf-8')
    for k in keys:
        if k not in done and k not in bfailed: bfailed[k] = 'no output from the translator'
    for k, why in sorted(bfailed.items()): print(f'[RED] {k}: {why}')   # before the post-pass, so a post-pass that stops doesn't lose them (#307's log had none of them)
    failed.update(bfailed)
    sys.stdout.flush()   # the post-pass writes to the same log
    return done, tout


def post_batch(a, done, tout, out, failed, many, j):
    """The post-pass of one batch (spv_android.py, j processes) into out. → the shaders made. With several batches it runs while the next batch translates"""
    if done:
        if not many: step('final pass')
        since = time.time() - 2   # exFAT keeps modification times to 2 s
        with counting(len(done), lambda: written(out, done, since)) if not many else contextlib.nullcontext():
            subprocess.run([sys.executable, str(HERE / 'spv_android.py'), '-j', str(j), str(tout), str(out)], check=True)   # every key in tout (this batch's): no list on the command line, which Windows limits to 32 K characters
    lost = [k for k in done if not (out / f'{k}.res.json').exists()]   # outputs the post-pass couldn't read (its [RED] lines): res.json, written last, makes a shader translated
    for k in lost: failed.setdefault(k, "the post-pass couldn't read its output")
    return len(done) - len(lost)


if __name__ == '__main__':
    main()
