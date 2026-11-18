# nxnative on Windows on Arm (arm64)

Status (branch `windows-arm`): **the host builds; it has not been run on a device yet.** `dmhost.exe` (with a window) and
`dmhost_cli.exe` (no window) cross-compile and link for arm64 Windows with llvm-mingw, from the Android port's Vulkan replay. The
only runs so far are the CI job on GitHub's `windows-11-arm` runners (`.github/workflows/windows-arm.yml`): the platform
checks (`woa_spike.exe`) and the self-test module in the host. No game has booted on Windows yet. The target is a Snapdragon
X machine (X Plus / X Elite, Adreno X1, Windows 11 24H2), such as a Surface with 16 GB of RAM. The first game is Pokémon
Pokopia (`0400A3A01EE4C000`, below). The design is [plan_windows_arm.md](plan_windows_arm.md).

Switch code is arm64 and runs natively, as it does on the Mac and on Android. This port reuses:

- `patch_nso.py`, the rewriter, with a third target (`DM_TARGET=windows`).
- The shared host (`dm/host`, `dm/gfx`).
- The Android port's Vulkan NVN replay (`dm/android/gfx_vk.c`) and its SPIR-V shaders.

Metal is not used. There is no DirectX path; Qualcomm's Windows driver provides Vulkan 1.3.

## Layout

```
dm/windows/
  CMakeLists.txt             dmhost.exe, dmhost_cli.exe, woa_spike.exe (cmake/aarch64-w64-mingw32.cmake: llvm-mingw)
  dmhost.manifest            UTF-8 code page, long paths, PerMonitorV2 DPI, segment heap, Windows 10/11
  compat/                    the Darwin and POSIX headers the host includes (dm_windows.h is force-included), over the UCRT and winpthreads
  posix_windows.c            mmap / mprotect / madvise on VirtualAlloc, with lazy commit; signals on a vectored exception handler;
                             pthread_kill by SuspendThread and context injection; files, directories and *at calls on UTF-16 Win32;
                             xattrs in NTFS alternate data streams; time; dlopen; string helpers
  plat_windows.c             the Mach and libdispatch pieces: thread suspend / get_state, vm_region, task_info, dispatch semaphores and queues;
                             SHA-256, CFDictionary; clonefile; section markers for swhle / swforce / swnvn
  plat_slots.c/.h            thread slots: TLS indices below 64, read by the rewritten code at x18 + 0x1480 + 8 * index (x18 is the TEB)
  coreaudio_wasapi.c         the AudioUnit API on WASAPI (shared mode, event driven, 20 ms, MMCSS "Pro Audio", device change handled)
  vk_driver_windows.c        vulkan-1.dll loaded at run time (DM_VK_DRIVER overrides), the FPCR guard around the driver
  front_windows.c            Win32 window + VK_KHR_win32_surface swapchain, status screen, XInput (1-4 pads, rumble), keyboard, mouse as touch
  media_windows.c            H.264 / AV1 / AAC stand-ins (no Media Foundation yet: those movies are skipped)
  tools/asm_coff.py          the host's Mach-O .S sources converted for the COFF assembler
  tools/spv_inc.py           glslang SPIR-V to the C arrays gfx_vk.c includes (in place of glslc -mfmt=c)
  tests/woa_spike.c          platform checks on the device (slots, fixed reservations, aliases, faults, lazy commit, code, suspension, PAC, stacks)
  tests/test_target_windows.py  the rewriter's Windows target on the synthetic self-test module (runs on any OS)
scripts/build_windows_from_nsp.py   NSP / XCI / NUP / dump folder -> a folder with dmhost.exe and game/
```

Shared files changed (guarded by `_WIN32` / `DM_X18_SLOTS`, so the Apple and Android builds compile the same code as before):

- `dm.h`, `hle.h`: `DM_X18_SLOTS` for Android or Windows; guest-facing `long` changed to `int64_t` (Windows is LLP64).
- `loader.c`: `# TARGET windows`, slots from `TlsAlloc`.
- `thread.c`: guest thread init.
- `hle_thread.c`, `crash.c`, `main.c`, `hle_fs.c`, `hle_time.c`, `hle_table.c`, `hle_net.c`, `hle_offline.c`, `hle_loopback.c`, `hle_nv.c`.
- `gfx_vk.c/.h`: the Win32 surface.
- `nso_asm.py`: the `windows` target.
- `build_android_from_nsp.py`: a hook that `build_windows_from_nsp.py` uses.

## How it maps to Windows

| Need | Apple / Android | Windows on Arm |
|---|---|---|
| The guest's thread slots | `tpidrro_el0` / an x18 block | the TEB's `TlsSlots` (x18 = TEB): `mov xt, x18; ldr xt, [xt, #0x1480 + 8*idx]` |
| The guest's own x18 | rewritten to slot K18 | the same; Windows never changes x18 under the guest |
| Fixed address space (Pokopia: 9.75 GB) | mmap | `VirtualAlloc` reserve; ranges of 64 MB and up are committed page by page on first touch (`DM_WIN_LAZY`), so a 16 GB PC is not charged 9 GB up front |
| Faults (dirty tracking, lazy commit, crash reports) | sigaction | a vectored handler that gives a `ucontext_t` over the Windows `CONTEXT` to the same handlers |
| Thread interruption (GC pause, preemption, hang reports) | pthread_kill / signals | `SuspendThread` + `GetThreadContext`; the handler is injected only when the thread is in guest code |
| Guest stacks (host-allocated) | — | TEB stack bounds opened per guest thread, so exception dispatch accepts them; `SetThreadStackGuarantee` |
| Aliased memory (`vm_remap`) | Mach / mremap | not available: it fails cleanly, as Android's does for private memory. Sparse NVN pools use the page table |
| Audio | CoreAudio / AAudio | WASAPI |
| GPU | Metal / Vulkan | Vulkan (`vulkan-1.dll`, Qualcomm Adreno X1 driver) |

## Building

On Linux, macOS or an x64 PC, cross-compiling. This is what CI does and what was tested:

```sh
# llvm-mingw: https://github.com/mstorsjo/llvm-mingw/releases (ucrt, for the build machine); glslang or the Vulkan SDK's glslc on PATH
pip install cmake ninja "capstone>=5,<6" lz4 zbic fonttools backports.zstd
cmake -G Ninja -S dm/windows -B build/win -DCMAKE_TOOLCHAIN_FILE=$PWD/dm/windows/cmake/aarch64-w64-mingw32.cmake \
      -DLLVM_MINGW=<llvm-mingw folder> -DGAME=0400A3A01EE4C000
ninja -C build/win dmhost_app dmhost_cli      # build/win/dmhost.exe, build/win/dmhost_cli.exe
cmake -G Ninja -S dm/windows -B build/spike -DCMAKE_TOOLCHAIN_FILE=... -DDM_SPIKE_ONLY=ON && ninja -C build/spike   # woa_spike.exe
python3 dm/windows/tests/test_target_windows.py -v     # with LLVM_MINGW set
```

On the Surface itself, use llvm-mingw's `aarch64` Windows release, plus Python 3.12 or later (arm64), CMake and Ninja (pip), and
glslang. The same commands apply; for CMake, give `-DLLVM_MINGW=C:\llvm-mingw`.

The executables are statically linked against winpthreads and the C++ runtime. They import only system DLLs (UCRT,
kernel32, user32, gdi32, ole32, winmm, avrt, xinput1_4, bcrypt). `vulkan-1.dll` is opened at run time.

## Packing a game (and Pokémon Pokopia)

You need your own dump of the game, your own keys if it is in NSPs, and the .NET SDK 10 (arm64) for the shader translator.
Nothing here downloads game files, keys or firmware.

Pokopia is packed from a **dump folder**:

- `base/exefs/` (`main`, `main.npdm`, `subsdk0`, `sdk`, ...)
- `updates/655360/exefs/` and `updates/655360/romfs/` (update v655360 = Ver. 2.0.1, whose RomFS is whole)

These are the version, module hashes and memory layout its profile (`dm/games/0400A3A01EE4C000/game.tsv`) registers:

- modules `main subsdk0 sdk`
- `mem_reserve 0x270000000`, `app_pool 0x240000000`, `mem_used 0x5600000`
- `depth_mode zero`
- `pad_layout full`

```powershell
# on the Surface (or any PC, then copy the output folder over)
python scripts\build_windows_from_nsp.py D:\dumps\pokopia --out D:\nxnative\pokopia --llvm-mingw C:\llvm-mingw
# NSPs instead:  python scripts\build_windows_from_nsp.py D:\nsp\pokopia -k D:\keys\prod.keys
# a NUP:         python scripts\build_windows_from_nsp.py D:\pokopia.nup
D:\nxnative\pokopia\dmhost.exe --console
```

The script runs the Android pipeline up to the shaders:

1. identify the game
2. extract it (`pack_dump.py` for a dump)
3. rewrite `main`, `subsdk0` and `sdk` with `DM_TARGET=windows`
4. split the 131,426 shaders and translate them to SPIR-V (`spv_build.py`)

Then it builds `dmhost.exe` for the profile and writes:

```
<out>\dmhost.exe  dmhost_cli.exe  env.txt  README.txt  selftest\
<out>\game\code\ (the rewritten modules)  RomFS.img  spv\  font\  aoc\  defaults.txt  game.json
```

- **Disk.** The shader translation needs about **90 GB or more free** (the profile's README measures that on a 16 GB Windows 11 PC). Use a large drive for
  the dump folder. With `--romfs link` (the default), the RomFS image is hard-linked from the extraction. `--romfs path` keeps it there and names it in
  `defaults.txt`.
- **Memory.** 9.75 GB of address space is reserved and committed only as touched. On 16 GB, keep the page file system-managed and close other large programs.
  `env.txt` `DM_OPMODE=0` (handheld 720p) lightens the GPU.
- **Movies.** Pokopia's movies are WebM/VP9 (`subsdk0`, libvpx, built in), so they play. H.264 / AV1 movies of other games are skipped for now.
- **First boot** shows the language list: `A`, `Up`, `A` (keyboard: `Z`, arrow up, `Z`). Then follow the Mac notes in the profile README.
- **Files.** The log is `log.txt` beside `dmhost.exe`. Saves, temp and pipeline cache are in `%LOCALAPPDATA%\nxnative\<title ID>`. Dumps from the `dump_now` switch go to `<out>\dump`.

### Rebuilding fast

A second build reuses everything that did not change: the extraction, the rewrite (`patched_windows_<version>/`), the shader split, every shader already
translated (by its hash, in `<id>_build\spv`) and the host's objects (Ninja). A failed test on the Surface does not cost the two hours of shaders again.

| Command | What runs |
|---|---|
| `build_windows_from_nsp.py <same input> --out <same folder>` | everything, but only what changed: shaders by hash; with the shader package unchanged, its 4 GB are not even read (`<id>_build\spv.stamp`; `DM_SPV_FORCE=1` reads them again) |
| `... --skip-shaders` | as above, the translated shaders taken as they are |
| `... --only host` (with `--out <folder>` or the input) | only `dmhost.exe` / `dmhost_cli.exe` rebuilt (changed sources only, about a second for one file) and copied into the folder of the last full build (`build_state.json` there); the game is left alone |
| `... --only stage` | the host, and the game folder synced again (changed files only) from the last build's outputs |
| `-j N` / `--jobs N` | shader translator processes and Ninja jobs (default: one per core) |

Shader translation:

- It runs in batches of `max(2000, 250 × jobs)` shaders (`DM_SPV_BATCH`), one .NET process per core.
- Each finished batch goes into `spv\` at once, so a stopped run keeps it, and the post-pass of one batch overlaps the translation of the next.
- The log has a line per batch with the rate and the minutes left. Reading a large shader package shows a progress count in MB.
- Temporary space covers one batch, not the whole game.
- Without `--jobs`, the number of processes is capped by free memory (700 MB each, `DM_SPV_MEM_MB`, keeping 2 GB free). A 16 GB Surface therefore does not page.

Faster disks and no scanning:

- **Internal NVMe, not USB.** Put the input's `<id>_build` folder and `--out` on the internal SSD. The split and the translation write and read over 100,000 small files.
- **Windows Defender exclusion.** Real-time scanning of each of those files is the largest slowdown on Windows. Exclude the build folder and the output folder. As administrator:
  `Add-MpPreference -ExclusionPath "D:\nxnative", "D:\dumps\pokopia\0400A3A01EE4C000_build"` (Settings › Privacy & security › Windows Security ›
  Virus & threat protection › Exclusions does the same). Alternatively, use a Dev Drive (Settings › System › Storage › Disks & volumes › Create Dev Drive), which defaults to
  asynchronous scanning.
- **Native arm64 tools on the Surface.**
  - Python for Windows arm64 (python.org's "ARM64" installer).
  - The .NET SDK 10 for Arm64: `dotnet --info` should say `RID: win-arm64`. An x64 dotnet runs the translator under emulation, several times slower.
  - llvm-mingw's `ucrt-aarch64` release, CMake and Ninja (`pip install cmake ninja` on arm64 Python).
  - glslang has no arm64 Windows release. The x64 one runs emulated and is used only for the host's few built-in shaders.
- **CPU tuning.** The host is compiled with `-mtune=oryon-1` (Snapdragon X). It uses only the armv8.2-a instructions every Windows on Arm PC has.
  `DM_CPU=oryon-1` (environment) also enables Snapdragon X's newer instructions; such a build then runs only there. `DM_TUNE=` (empty) keeps the compiler's default.

Controls (the defaults; F2 → Controls changes them):

| Control | Input |
|---|---|
| Xbox / XInput pads | players 1-4, with rumble; positional A/B/X/Y (`pad_abxy=label` for by label) |
| Left stick | WASD |
| Right stick | IJKL |
| D-pad | arrows |
| A / B / X / Y | Z / X / C / V |
| L / R | E / U |
| ZL / ZR | Q / O |
| Stick clicks | F / H |
| + / − | Enter / Backspace |
| Touch | the mouse, or the touch screen where no control is |
| Touch controls on / off | F1 |
| Settings menu | F2 |
| Full screen | F11 or Alt+Enter |

### Touch controls and settings

On a touch screen (Surface) the Switch's controls are drawn over the game, semi-transparent: both sticks, the D-pad, A/B/X/Y,
L/R/ZL/ZR, +/− and the stick clicks (LS/RS). Several fingers work at once, and a finger can slide from one button to the next.
A touch on no control goes to the game's own touch screen. In `auto` mode the controls hide as soon as a key or the controller
is used, and come back with the next touch. F1 shows or hides them and remembers the choice.

Settings live in `%LOCALAPPDATA%\nxnative\<title ID>\settings.ini`, one `key=value` per line. The program writes this file
itself, and each key can also be given in `env.txt` as `DM_<KEY>`, for example `DM_TOUCH_OPACITY=0.4`:

| Key | Values | Default |
|---|---|---|
| `touch_overlay` | `auto` (shown on a touch screen, hidden while a key or the controller is used), `on`, `off` | `auto` |
| `touch_opacity` | 0 to 1 | 0.6 |
| `touch_scale` | 0.5 to 2 | 1 |
| `touch.<control>` | `x,y[,r]`: units of the window's shorter side, negative counts from the right or bottom edge; `r` 0 hides it. Controls: `lstick rstick dpad a b x y l r zl zr plus minus ls rs` | built-in layout |
| `stall_note` | seconds without a new frame before the "Still working" note shows (0: never) | 3 |

### Graphics and the settings menu (F2)

F2 opens the settings menu over the game. Touch, the mouse, the arrow keys with Enter, or the controller's D-pad with A all work
in it; Esc, F2 or B closes it. The game gets no input while it is open. Settings marked `*` apply at the next start, and the
menu's "Restart the game now" entry restarts the game for you, once the old run has ended and saved.

| Key | Values | Applies | Effect |
|---|---|---|---|
| `vsync` | `1`, `0` | at once | `0`: frames as fast as the GPU goes (mailbox / immediate), may tear |
| `fps_target` | unset (the game's profile), `30`, `60`, `0` (the game's own limit) | next start | frame cap (`DM_FPS_TARGET`) |
| `opmode` | unset, `1` docked, `0` handheld | next start | handheld: most Switch 2 games render at the lower handheld resolution, which is the biggest speed-up (`DM_OPMODE`) |
| `render_scale` | unset, `0.75`, `0.5` | next start | only games whose build has a render-scale patch (`DM_RENDER_SCALE`) |
| `async_shaders` | unset (on), `0` | next start | pipelines built in the background, so fewer stutters (`DM_VK_ASYNC_PIPE`) |
| `filter` | `linear`, `nearest` | at once | the picture's scaling to the window: smooth or sharp |
| `show_fps` | `0`, `1` | at once | frames shown per second, at the top left |

The NVN replay has no general internal-resolution scale, so a 1.5x upscale isn't offered. The handheld mode is the resolution
setting that works for every game.

Example: `touch.a=-0.12,-0.6,0.07` makes A bigger, and `touch.ls=0,0,0` hides the stick click.

Until the first frame the window shows a loader: the game's name, a spinner, the current phase (modules, memory, graphics,
pipeline cache...), the elapsed time and the number of shaders and pipelines compiled so far. When the picture stops changing
for a few seconds later on (a level loading, shaders compiling), a small "Still working" note with a spinner appears at the top.

### The launcher (nxlauncher.exe)

`nxlauncher.exe` sits beside `dmhost.exe` in every built folder. It shows the covers of the built games: each game's own icon
(`game\icon.jpg`, from its NACP control data), with its name, title ID and version. A tap, a click, Enter or the controller's A
starts that game's `dmhost.exe`. The launcher then minimizes and comes back when the game ends. Drag or use the wheel to scroll.
A right click or a long press opens the game's menu: start, its folder, its log.txt, its saves and settings.

It looks in the folders of its list, in each folder itself and in the folders right inside it. The list starts with its own folder and
the one above it, so `D:\nxnative_pokopia\nxlauncher.exe` finds every `D:\<name>` game folder. "+ Add folder" adds another one,
and "Folders" removes one. The list and the last-played order are kept in `%LOCALAPPDATA%\nxnative\launcher.ini`.
`nxlauncher.exe --folder <path>` adds a folder for that run only. Pin it to Start or the taskbar like any program.

### Typing text: the game's software keyboard

When the game opens its keyboard (a name, a search), a box opens over the window with the game's prompt: type with the keyboard
and press Enter (OK) or Esc (Cancel); Cancel and the window's X send the game a cancel, as the Switch does. **⌨ Touch** opens
Windows' touch keyboard; in tablet posture (the Surface's keyboard folded back or detached) it opens by itself. A minimum length
the game sets keeps OK greyed out until it is reached. The box runs on a thread of its own: a game thread's stack is a few KB,
and the window and its text services ran past it (the program closed without a line). `DM_TEXT_TEST=<s>` (with `DM_TEXT_AUTO=<text>`)
checks it without a game: the box is asked for from a 16 KB stack after `<s>` seconds and answered by itself (CI).

### Controls: changing the keys and buttons, rumble

F2 → **Controls** lists the Switch's buttons with their keyboard key and Xbox controller button. Choose a row (tap, click, or
arrows + Enter, or the D-pad + A), then press the key or the controller button it should have. Esc keeps the old one and Delete
leaves the button without one. A key or button already used elsewhere is swapped, so nothing is lost. The two "stick" rows ask
for the keyboard's four direction keys in turn (up, down, left, right). In the menu the controller always works as pressed
(D-pad, A, B), whatever the mapping, so a wrong mapping can still be fixed.

"Changes are saved for" picks **this game** (`%LOCALAPPDATA%\nxnative\<title ID>\settings.ini`) or **every game**
(`%LOCALAPPDATA%\nxnative\settings.ini`; a game's own setting wins). "Reset to the defaults" (tap twice) clears the chosen one.
The lines can also be written by hand:

| Key | Value | Example |
|---|---|---|
| `key.<button>` | key names separated by spaces, or `none`. Buttons: `a b x y l r zl zr ls rs plus minus up down left right lstick_up lstick_down lstick_left lstick_right rstick_up ...`. Keys: letters, digits, `Enter Space Tab Backspace Shift Ctrl Alt Up Down Left Right Num0`..`Num9` `Num+ Num- F3`..`F24` `; + , - . /`, or a code like `0xDE` | `key.a=Space K` |
| `pad.<Xbox input>` | a Switch button (above, not the stick directions) or `none`. Inputs: `a b x y lb rb lt rt ls rs start back up down left right` | `pad.a=a` |
| `pad_abxy` | `position` (Xbox A = the Switch's B, the default) or `label` (Xbox A = A): the default Xbox mapping | `pad_abxy=label` |
| `rumble` | `1`, `0.5` (weak), `0` (off); the menu changes it at once | `rumble=0.5` |

F1, F2, F11 and Esc stay the program's own keys.

### A faster second start (pipeline cache)

The Vulkan pipelines the driver compiles are kept in `%LOCALAPPDATA%\nxnative\<title ID>\cache\pipeline.cache.*`, so the second
start skips most of the compiling. The file is written every 64 new pipelines (at most every 15 s), when the window is minimized,
and when the game closes; always through a temporary file, so a crash never leaves half a cache. At start its header is checked
against the GPU and driver (vendor, device and the driver's cache UUID): after a driver update, or with a damaged file, the log says
`[vk] pipeline cache ... another driver version's` and it starts empty, then fills again.

### When the GPU hangs (Windows' TDR)

Windows resets a GPU that stays on one submission for more than about 2 s (TDR). Qualcomm's driver then refuses every later
submission with `-3`, not `VK_ERROR_DEVICE_LOST`, and the picture froze for good. Now the first refused submission after a cut
one counts as a lost GPU, and so do 8 refusals in a row. `vk_start.txt` gets `device_lost=hang (submission N, M ms)` (or the
VkResult), the log says `the GPU is gone`, and a message offers to start the game again (saves kept). `DM_GPU_LOST_NOASK=1`
closes without asking.

To narrow such a hang down (env.txt):

| Line | What it does |
|---|---|
| `DM_VK_ROBUST=1` (or menu: GPU safe reads) | out-of-range buffer / image reads in shaders give 0 (robustBufferAccess, robustness2); a little slower. On by default for Pokémon Pokopia, whose world's draws hung the Adreno X1-45 without it (`DM_VK_ROBUST=0` turns it off) |
| `DM_SPV_CB_BOUND=0` | turns off the bound on uniform buffer reads at a computed index (on by default, every GPU: such a read gives 0 past the 64 KB array, as on the Switch; Pokémon Pokopia's world shader hung the Adreno X1-45 reading gigabytes past it) |
| `DM_VK_SPLIT_SHADER=<hash>[,<hash>]`, `DM_VK_SPLIT_DRAWS=<n>` | sends the chunk to the GPU after every n (default 8) draws of a listed vertex or fragment shader: a test for a GPU hang in a long run of one shader's draws (Pokémon Pokopia's world, `85c01ed67ec089e9`), cheaper than `DM_VK_MID_DRAWS=1` for every draw. Off by default |
| `DM_VK_CRUMB_DETAIL=1` | the GPU-hang report lists, under every draw, its indices (count, base vertex, vertex range), first vertex buffer, the textures it samples (serial, size, format; `!FB` when one is an attachment of its own pass) and its targets. On by itself for the draws of a `DM_VK_SPLIT_SHADER` shader. A draw that samples an attachment of the pass it draws in (a feedback loop) is always named in a `[RED] … feedback loop` line |
| `DM_VK_SKIP_DRAW=<vertex shader>:<vertices>[,…]` | leaves out only the draws of that vertex shader with that vertex (index) count, as the hang report lists them: one mesh, to find the draw the GPU hangs in (Pokopia: `85c01ed67ec089e9:1620`) |
| `DM_SPV_LOOP_CAP=<n>` (menu: Shader loop limit) | every shader loop stops after n trips (default on the Adreno: 4096; `0` off). Applied as the shaders load: no new translation |
| `DM_VK_STORAGE_UP=2` | an sRGB image a shader stores to is stored through its UNORM twin (the Adreno cannot store sRGB). On by default for Pokémon Pokopia, whose 3D world was black without it |
| `DM_SPV_DEMOTE=1` | a fragment shader's discard (OpKill) becomes a demote to a helper invocation and a return: an A/B for a driver that mishandles OpKill |
| `DM_VK_SKIP_SHADER=<hash>,<hash>` | draws of these shaders (the hang report names them) are left out: a way to keep playing past one that hangs |
| `DM_VK_MID_DRAWS=8` | GPU submissions of 8 draws, so the hang report names the draw closely (slower) |
| `DM_OVERLAY_WINDOW=0` | no window over the game at all: no touch controls, F2 menu, FPS or notes |
| `DM_TOUCH_OVERLAY=off` | the touch controls hidden (F1 brings them back); the menu still works |

The hang report lists the submission's passes, draws, dispatches and (now) its copies, uploads, blits and clears. It also says whether the
submission ever started on the GPU (its start timestamp written): one that never started waited behind other work, so its own
draws are not the cause. While the GPU is stuck on a submission for 2 s or more, the note over the game says `Waiting for the GPU`
instead of `Still working` (whose shader and pipeline numbers are totals since the start, not work in progress).

The touch controls, menu and notes are a separate GDI window composed by Windows. They never submit Vulkan work.

## Tested so far

| What | Where | Result |
|---|---|---|
| `dmhost.exe`, `dmhost_cli.exe`, `woa_spike.exe` compile and link (arm64 PE, `-Werror` on pointer/int casts and implicit declarations) | Linux, llvm-mingw 20261104 | yes |
| Imports only system DLLs; manifest embedded; section markers for `swhle` / `swforce` / `swnvn` sized as the objects' | Linux (llvm-readobj) | yes |
| Rewriter `DM_TARGET=windows` on the self-test module: layout says `windows`, no system register or svc left, slot base `mov xt, x18`, code identical to Android's | Linux (`test_target_windows.py`) | 4/4 pass |
| `build_windows_from_nsp.py --host-only` (CMake configure, build, self test, folder) | Linux | yes |
| `woa_spike.exe` and `dmhost_cli.exe selftest main` | `windows-11-arm` CI | see the latest workflow run |
| A game | — | **not run yet** |

## What is missing, and the risks

The risks are ordered by how likely they are to block Pokopia:

1. **No device run yet.** Everything past the CI self test is unverified: memory layout, VEH paths, audio, the swapchain on the Adreno X1 driver, input.
2. **Qualcomm's Windows Vulkan driver.** The replay was tuned on Android Adreno drivers (`spv_android.py` rewrites). The Windows driver
   differs in extensions and bugs. Whether it needs those rewrites, or others, is unknown until a run.
3. **Guest stack and exception edge cases.** The guest has no stack probes, and Windows dispatches exceptions on the faulting stack.
   Mitigations:
   - the stack bounds are opened per guest thread
   - `SetThreadStackGuarantee`
   - a context injection frame of 1 KB below sp

   `brk #0xF003` (fast fail) and PAC-signed return addresses in exception unwinding are not exercised.
4. **No aliasing (`vm_remap`).** This matches Android, where it fails as well. Sparse NVN pools go through the page table; JIT
   double-mapping is not available.
5. **Media Foundation not ported.** H.264 / AV1 / AAC movies are skipped (not an issue for Pokopia).
6. **Shader translation** needs the .NET SDK 10 (`dmsh_xlate`), about 90 GB free, and hours of CPU time for Pokopia's 131,426 shaders.
7. **No networking.** Sockets return `EAFNOSUPPORT`, which is fine for offline play.
8. **No launcher UI or packaged installer.** The folder runs as is.
