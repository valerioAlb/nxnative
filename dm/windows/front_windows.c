// Windows front end (dmhost.exe, the GUI program): the game's window, its input, focus and lifecycle, and the log, around the host's main() (main.c), as the
// Android port's front_android.c does on a phone.
//   Layout beside the program (scripts/build_windows_from_nsp.py makes it):
//     dmhost.exe, env.txt (KEY=VALUE per line, as on Android and iOS), game/ — code/ (the modules rewritten with DM_TARGET=windows: <name>.patched.bin, .fixups.tsv,
//     .gates.tsv ...), RomFS/ or RomFS.img, spv/ (the shaders translated for Vulkan), font/, aoc/, defaults.txt; and selftest/ (the self test's module, run without a game).
//   Save data, the pipeline cache and temp files: %LOCALAPPDATA%\nxnative\<title ID>\ (saves/, cache/, temp/). Log: log.txt beside the program (the previous one kept
//   as log.prev.txt), and the console's stderr when started from one (dmhost.exe --console opens one).
//   Window: Vulkan (VK_KHR_win32_surface) on the NVN replay's device (gfx_vk.c); the window texture the game presented last is drawn scaled to fit; before the first,
//   a status screen. F11 or Alt+Enter: full screen (borderless). The window's close ends the game as SIGTERM does (hle_lifecycle.c), saving what the game saved.
//   Input: an XInput controller (Xbox layout, buttons by position: the Switch's A is the right face button; DM_PAD_ABXY=label maps by label) is player 1, with
//   rumble; more controllers are players 2-4. The keyboard is player 1 too (Ryujinx's default keys): left stick WASD, right stick IJKL, D-pad arrows,
//   A Z, B X, X C, Y V, L E, R U, ZL Q, ZR O, + Enter or =, - Backspace or -, stick clicks F and H. The mouse's left button and a touch on no control are the touch screen;
//   the touch controls (overlay_windows.c, F1) are drawn over the picture. Until the first frame: the loader (status_draw); a long stall later: a note (stall_note).
//   Settings: settings.ini in the data folder (settings_windows.c)
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <windowsx.h>
#include <xinput.h>
#include <shlobj.h>
#include <shellapi.h>
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include "nvn_obj.h"
#include "gfx_vk.h"
#include <vulkan/vulkan_win32.h>   // (the SDK header: its types only; the call goes through the instance)
#include "game_profile.h"
#include "hle_haptics.h"
#include "winfront.h"
#include "touchpad.h"
#include <stdatomic.h>
#include <math.h>
#include <sys/stat.h>

int main(int argc, char **argv);   // main.c
void dm_focus_set(int st);
void dm_audio_suspend(int on);
void guest_clock_freeze_for(int reason, int on);
void dm_win_post_term(void);       // posix_windows.c: SIGTERM for hle_lifecycle.c's waiter
PFN_vkGetInstanceProcAddr dm_vk_instance_proc(void);   // vk_driver_windows.c

enum { ST_START, ST_SELFTEST, ST_PASS, ST_FAIL, ST_GAME, ST_ENDED, ST_CRASHED, ST_DEMO };
static _Atomic int g_state = ST_START;
static _Atomic uint64_t g_frames;
static char g_dir[600], g_game[700], g_data[700], g_last[200], g_phase[80] = "Starting";
static uint64_t g_t0;   // the program's start (dm_win_mono_ns)
void mtl_create_stats(uint64_t *modules, uint64_t *pipelines);
double mtl_gpu_wait_s(void);   // gfx_vk.c: seconds the GPU has been on its oldest submission in flight (0: none)   // gfx_vk.c: shader modules and pipelines made so far
static pthread_mutex_t g_txt_mu = PTHREAD_MUTEX_INITIALIZER;
static HWND g_wnd;
static _Atomic int g_closing;

// ---------- Log: stderr → log.txt, the console (if any) and the debugger ----------
// stderr (and stdout) are log.txt itself, opened before anything else runs and unbuffered: every line is in the file the moment it is written, so a start
// that dies at once (a crash in a driver, an abort) still leaves its lines. A GUI program's CRT streams have no file behind them (UCRT: descriptor -2),
// so writing to them was lost: log.txt stayed empty (Valerio's Surface: no log at all). A thread follows the file for the console and the window's state.
static HANDLE g_console_err;
static wchar_t g_logw[MAX_PATH + 16];
static int g_console;   // a console shows the log (--console, or started from one)
static void log_line(char *s) {
    if (g_console_err) { DWORD w; WriteFile(g_console_err, s, (DWORD)strlen(s), &w, NULL); WriteFile(g_console_err, "\r\n", 2, &w, NULL); }
    OutputDebugStringA(s); OutputDebugStringA("\n");
    if (strstr(s, "selftest: PASS")) atomic_store(&g_state, ST_PASS);
    else if (strstr(s, "selftest: FAIL")) atomic_store(&g_state, ST_FAIL);
    if (strstr(s, "CRASH") || strstr(s, "FATAL")) atomic_store(&g_state, ST_CRASHED);
    static const struct { const char *pat, *phase; } ph[] = {
        {"game in ", "Reading the game's code"}, {"modules:", "Loading the game's modules"}, {"reloc ", "Linking the game's modules"},
        {">>> nnosInitialize", "Starting the system services"}, {"[mem] heap", "Setting up the game's memory"}, {"DT_INIT", "Running the game's initializers"},
        {">>> nnMain", "Starting the game"}, {"fs mount rom", "Opening the game's data"}, {"WindowInitialize", "Setting up the graphics"},
        {"chunks in flight", "Setting up the graphics"}, {"pipeline cache:", "Loading the pipeline cache"}, {"OpenAudioOut", "Starting the audio"},
        {"fs mount save", "Opening the save data"}, {"colour target", "Drawing the first frames"},
    };
    const char *np = NULL; for (size_t i = 0; i < sizeof ph / sizeof ph[0]; i++) if (strstr(s, ph[i].pat)) { np = ph[i].phase; break; }
    pthread_mutex_lock(&g_txt_mu); snprintf(g_last, sizeof g_last, "%s", s); if (np) snprintf(g_phase, sizeof g_phase, "%s", np); pthread_mutex_unlock(&g_txt_mu);
}
static void *log_follow(void *p) {
    (void)p;
    HANDLE h = CreateFileW(g_logw, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char buf[8192]; size_t n = 0;
    for (;;) {
        DWORD k = 0;
        if (!ReadFile(h, buf + n, (DWORD)(sizeof buf - 1 - n), &k, NULL)) break;
        if (!k) { Sleep(15); continue; }
        n += k; buf[n] = 0;
        char *s = buf, *e;
        while ((e = memchr(s, '\n', buf + n - s))) { *e = 0; if (e > s && e[-1] == '\r') e[-1] = 0; log_line(s); s = e + 1; }
        n -= (size_t)(s - buf); memmove(buf, s, n);
        if (n == sizeof buf - 1) n = 0;
    }
    CloseHandle(h);
    return NULL;
}
static int log_open(void) {   // log.txt (made empty) as stderr and stdout: → 1 when it could be written
    FILE *f = _wfopen(g_logw, L"wb"); if (!f) return 0;
    fprintf(f, "[dm] nxnative starting (dmhost.exe, %s)\n", DM_GAME_ID); fclose(f);
    if (!_wfreopen(g_logw, L"ab", stderr)) return 0;
    if (_fileno(stderr) != 2 && _fileno(stderr) >= 0) _dup2(_fileno(stderr), 2);   // raw writes to descriptor 2 too
    setvbuf(stderr, NULL, _IONBF, 0);
    if (_wfreopen(g_logw, L"ab", stdout)) { setvbuf(stdout, NULL, _IONBF, 0); if (_fileno(stdout) != 1 && _fileno(stdout) >= 0) _dup2(_fileno(stdout), 1); }
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    return 1;
}
static void log_start(int console) {
    if (console && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) { g_console_err = GetStdHandle(STD_ERROR_HANDLE); SetConsoleOutputCP(CP_UTF8); }
    else if (AttachConsole(ATTACH_PARENT_PROCESS)) { g_console_err = GetStdHandle(STD_ERROR_HANDLE); SetConsoleOutputCP(CP_UTF8); }
    if (g_console_err == INVALID_HANDLE_VALUE) g_console_err = NULL;
    if (g_console_err) {   // AttachConsole leaves the std handles as they were (log.txt's): the console's own output buffer
        HANDLE c = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        g_console_err = c != INVALID_HANDLE_VALUE ? c : NULL;
    }
    g_console = g_console_err != NULL;
    pthread_t th; if (!pthread_create(&th, NULL, log_follow, NULL)) pthread_detach(th);
}

// ---------- Fatal errors: crash.txt, and a message box when no console shows the log ----------
static _Atomic int g_fatal_shown;
static void fatal_note(const char *what) {   // posix_windows.c dm_win_fatal_hook: an exception nothing handled, or abort(); runs on the failing thread
    if (atomic_exchange(&g_fatal_shown, 1)) return;
    fprintf(stderr, "[dm] FATAL %s\n", what);
    wchar_t cw[MAX_PATH + 16]; wcscpy(cw, g_logw); wchar_t *sl = wcsrchr(cw, L'\\'); if (sl) wcscpy(sl + 1, L"crash.txt");
    FILE *c = _wfopen(cw, L"wb");
    if (c) {   // the error, then the log's last 32 KB
        fprintf(c, "nxnative %s (%s) stopped: %s\r\nThe log up to then (all of it in log.txt):\r\n\r\n", DM_GAME_NAME, DM_GAME_ID, what);
        FILE *l = _wfopen(g_logw, L"rb");
        if (l) { fseek(l, 0, SEEK_END); long n = ftell(l); fseek(l, n > 32768 ? n - 32768 : 0, SEEK_SET); char b[4096]; size_t k; while ((k = fread(b, 1, sizeof b, l))) fwrite(b, 1, k, c); fclose(l); }
        fclose(c);
    }
    if (g_console_err) { DWORD w; char m[600]; int n = snprintf(m, sizeof m, "\r\n[dm] FATAL %s\r\n(see log.txt and crash.txt beside dmhost.exe)\r\n", what); WriteFile(g_console_err, m, (DWORD)n, &w, NULL); }
    const char *nb = getenv("DM_NO_FATAL_BOX");
    if (!(nb && atoi(nb))) {   // a double-click shows no console: say it in a box (DM_NO_FATAL_BOX=1: never, for unattended runs)
        char m[900]; snprintf(m, sizeof m, "nxnative stopped:\n\n%s\n\nlog.txt and crash.txt beside dmhost.exe have the details: send them along when reporting it.", what);
        wchar_t wm[900]; MultiByteToWideChar(CP_UTF8, 0, m, -1, wm, 900);
        MessageBoxW(NULL, wm, L"nxnative", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }
}

// ---------- Files ----------
static int exists(const char *p) { struct stat st; return !stat(p, &st); }
static void mkdirs(const char *p) {
    char b[800]; snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if ((*s == '/' || *s == '\\') && s[-1] != ':') { char c = *s; *s = 0; mkdir(b, 0755); *s = c; }
    mkdir(b, 0755);
}
static void env_file(const char *dir, const char *name, int over) {
    char path[800]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f && over) {   // env.txt: Explorer hides the extension, so a file made as a text document is env.txt.txt
        char alt[820]; snprintf(alt, sizeof alt, "%s.txt", path);
        if ((f = fopen(alt, "r"))) LOG("%s: read from %s (the name has .txt twice: Explorer hides extensions)", name, alt);
        else LOG("%s: none at %s", name, path);
    }
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0; if (!over && getenv(line)) continue;
        setenv(line, eq + 1, 1); LOG("%s: %s=%s", name, line, eq + 1);
    }
    fclose(f);
}
static void keep_prev(const char *path) {   // log.txt → log.prev.txt before the new start writes it
    char p[800]; const char *dot = strrchr(path, '.');
    if (!dot) return;
    snprintf(p, sizeof p, "%.*s.prev%s", (int)(dot - path), path, dot);
    if (exists(path)) rename(path, p);
}

// ---------- The guest ----------
static int g_argc; static char *g_argv[8];
static void *guest_thread(void *p) {
    (void)p;
    int rc = main(g_argc, g_argv);
    LOG("host main returned %d", rc);
    for (int i = 0; i < 200 && atomic_load(&g_state) == ST_SELFTEST; i++) Sleep(10);
    if (atomic_load(&g_state) == ST_PASS && getenv("DM_NVN_DEMO")) {   // the picture path without a game: NVN calls → the Vulkan replay → this window
        int nvn_demo_run(int frames);
        atomic_store(&g_state, ST_DEMO); int n = nvn_demo_run(atoi(getenv("DM_NVN_DEMO")) > 0 ? atoi(getenv("DM_NVN_DEMO")) : 600);
        LOG("NVN picture check: %d frames presented", n);
        atomic_store(&g_state, n > 0 ? ST_PASS : ST_FAIL);
    }
    int st = atomic_load(&g_state); if (st == ST_GAME || st == ST_SELFTEST) atomic_store(&g_state, ST_ENDED);
    if (g_wnd && st == ST_GAME) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    return NULL;
}
void front_run(void (*guest)(void)) { guest(); dm_slots_x18_restore(); }
static void start_host(const char *game_arg) {
    char code[800], buf[800];
    if (game_arg) snprintf(g_game, sizeof g_game, "%s", game_arg);
    snprintf(code, sizeof code, "%s/code", g_game);
    snprintf(buf, sizeof buf, "%s/saves", g_data); mkdirs(buf);
    snprintf(buf, sizeof buf, "%s/saves/%s", g_data, DM_GAME_ID); setenv("DM_SAVE", buf, 0);
    snprintf(buf, sizeof buf, "%s/temp", g_data); mkdirs(buf); setenv("DM_TEMP", buf, 0);
    snprintf(buf, sizeof buf, "%s/cache", g_data); mkdirs(buf);
    snprintf(buf, sizeof buf, "%s/cache/pipeline.cache", g_data); setenv("DM_PIPELINE_CACHE", buf, 0);
    setenv("DM_AUDIO", "1", 0);
    setenv("DM_OPMODE", "1", 0);   // a PC is docked: the TV's 1080p (env.txt DM_OPMODE=0 for handheld's 720p, lighter on the GPU)
    snprintf(buf, sizeof buf, "%s/dump", g_dir); setenv("DM_DUMP_DIR", buf, 0); mkdirs(buf);
    snprintf(buf, sizeof buf, "%s/main.fixups.tsv", code);
    if (exists(buf) && !getenv("DM_FORCE_SELFTEST")) {
        setenv("DM_ROOT", g_game, 0);
        snprintf(buf, sizeof buf, "%s/RomFS", g_game);
        if (!exists(buf)) snprintf(buf, sizeof buf, "%s/RomFS.img", g_game);
        if (getenv("DM_ROMFS")) LOG("RomFS: %s (env.txt / defaults.txt)", getenv("DM_ROMFS"));   // build_windows_from_nsp.py --romfs path names the extraction's image
        else if (exists(buf)) setenv("DM_ROMFS", buf, 0);
        else LOG("[RED] no RomFS in %s (RomFS/ or RomFS.img): the game will find no files", g_game);
        snprintf(buf, sizeof buf, "%s/aoc", g_game); if (exists(buf)) setenv("DM_AOC_DIR", buf, 0);
        snprintf(buf, sizeof buf, "%s/spv", g_game); setenv("DM_SPV_DIR", buf, 0);
        snprintf(buf, sizeof buf, "%s/glslc_capture", g_game); setenv("DM_GLSLC_CAPTURE", buf, 0);
        snprintf(buf, sizeof buf, "%s/font", g_game); if (exists(buf)) setenv("DM_FONT_DIR", buf, 0);
        snprintf(buf, sizeof buf, "%s/font/standard.ttf", g_game);
        if (!exists(buf)) {
            char sf[MAX_PATH * 3] = ""; wchar_t w[MAX_PATH];
            if (GetWindowsDirectoryW(w, MAX_PATH)) { char wd[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, w, -1, wd, sizeof wd, NULL, NULL); snprintf(sf, sizeof sf, "%s/Fonts/arial.ttf", wd); }
            if (sf[0] && exists(sf)) setenv("DM_SHARED_FONT", sf, 0);
        }
        g_argv[0] = "dmhost"; g_argv[1] = _strdup(code); g_argc = 2;
        atomic_store(&g_state, ST_GAME); LOG("game in %s", code);
    } else {
        snprintf(buf, sizeof buf, "%s/selftest/main.fixups.tsv", g_dir);
        if (!exists(buf)) { LOG("[RED] no game in %s and no self test in %s/selftest", code, g_dir); atomic_store(&g_state, ST_FAIL); return; }
        snprintf(buf, sizeof buf, "%s/selftest", g_dir);
        setenv("DM_DBG", "1", 1);
        g_argv[0] = "dmhost"; g_argv[1] = _strdup(buf); g_argv[2] = "main"; g_argc = 3;
        atomic_store(&g_state, ST_SELFTEST); LOG("no game in %s: running the self test", code);
    }
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_thread, NULL)) { LOG("[RED] cannot start the guest thread"); atomic_store(&g_state, ST_FAIL); return; }
    pthread_detach(th);
}

// ---------- Front end interface (front.h) ----------
void *front_metal_layer(void) { return NULL; }
int front_has_window(void) { return 1; }
int front_screen_max_fps(void) { HDC dc = GetDC(NULL); int hz = dc ? GetDeviceCaps(dc, VREFRESH) : 0; if (dc) ReleaseDC(NULL, dc); return hz > 1 ? hz : 0; }
int front_ctrl_support(int min, int max) { (void)min; (void)max; return -1; }
void front_screen_match(int fps) { (void)fps; }
void front_progress(const char *title, long done, long total) { if (title) LOG("progress: %s %ld / %ld", title, done, total); }
// The software keyboard: a small input box (the game's guide text as its prompt). UTF-8 both ways
static wchar_t g_ti_text[512]; static const wchar_t *g_ti_title;
static LRESULT CALLBACK ti_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) { GetDlgItemTextW(h, 100, g_ti_text, 512); PostMessageW(h, WM_USER, 1, 0); return 0; }
        if (LOWORD(w) == IDCANCEL) { PostMessageW(h, WM_USER, 0, 0); return 0; }
        break;
    case WM_CLOSE: PostMessageW(h, WM_USER, 0, 0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)sub;
    static ATOM cls; HINSTANCE hi = GetModuleHandleW(NULL);
    if (!cls) { WNDCLASSW wc = {.lpfnWndProc = ti_proc, .hInstance = hi, .lpszClassName = L"dmtext", .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .hCursor = LoadCursor(NULL, IDC_ARROW)}; cls = RegisterClassW(&wc); }
    wchar_t wt[256] = L"", wg[256] = L"", wi[512] = L"";
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, wt, 256); MultiByteToWideChar(CP_UTF8, 0, guide && *guide ? guide : title ? title : "", -1, wg, 256);
    MultiByteToWideChar(CP_UTF8, 0, initial ? initial : "", -1, wi, 512);
    g_ti_title = wt;
    HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"dmtext", wt[0] ? wt : L"Text", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 480, 160, g_wnd, NULL, hi, NULL);
    if (!d) return -1;
    CreateWindowExW(0, L"STATIC", wg, WS_CHILD | WS_VISIBLE, 12, 10, 440, 20, d, NULL, hi, NULL);
    HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wi, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 36, 440, 26, d, (HMENU)100, hi, NULL);
    if (max_chars > 0) SendMessageW(e, EM_LIMITTEXT, (WPARAM)max_chars, 0);
    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 270, 76, 85, 28, d, (HMENU)IDOK, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 365, 76, 85, 28, d, (HMENU)IDCANCEL, hi, NULL);
    SetFocus(e); SendMessageW(e, EM_SETSEL, 0, -1);
    MSG msg; int r = -1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd == d && msg.message == WM_USER) { r = (int)msg.wParam; break; }
        if (!IsDialogMessageW(d, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DestroyWindow(d);
    if (r != 1) return -1;
    WideCharToMultiByte(CP_UTF8, 0, g_ti_text, -1, out, out_cap, NULL, NULL);
    return 0;
}
void front_android_present(const tex_t *t, const int crop[4]) { (void)t; (void)crop; atomic_fetch_add(&g_frames, 1); }
uint64_t front_vsync(uint64_t *refresh) { *refresh = 0; return 0; }   // no VK_GOOGLE_display_timing on Windows: frames are paced on the host's clock

// ---------- Vulkan ----------
static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkSurfaceKHR g_surf; static VkSwapchainKHR g_sc; static VkImage g_img[8]; static uint32_t g_nimg; static VkFormat g_fmt; static VkExtent2D g_ext2;
static int g_sc_srgb, g_sc_has_srgb, g_sc_stale;
static VkCommandPool g_cpool; static VkCommandBuffer g_cb; static VkBuffer g_stg; static VkDeviceMemory g_stgm; static void *g_stgp; static VkDeviceSize g_stg_sz;
static VkFence g_fence; static VkSemaphore g_acq, g_done;
static int g_pic[4];   // the picture's rectangle in the window (x, y, w, h), for the mouse's touches
static int vk_bad(const char *call, VkResult r) { LOG("[RED] Vulkan: %s = %s", call, vk_result_str(r)); return 0; }
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) return vk_bad(#x, r_); } while (0)
static void vk_probe(void) {
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(g_pd, &pr);
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(g_pd, &f);
    LOG("vulkan: %s, API %u.%u.%u, driver %#x, vendor %#x device %#x", pr.deviceName, VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion),
        VK_API_VERSION_PATCH(pr.apiVersion), pr.driverVersion, pr.vendorID, pr.deviceID);
    LOG("vulkan textures: BC %d, ETC2 %d, ASTC LDR %d; geometry shader %d, tessellation %d, storage image write without format %d, multi viewport %d",
        f.textureCompressionBC, f.textureCompressionETC2, f.textureCompressionASTC_LDR, f.geometryShader, f.tessellationShader, f.shaderStorageImageWriteWithoutFormat, f.multiViewport);
}
static int vk_init(void) {
    if (g_inst) return 1;
    vkb_ctx *c = vkb();
    if (!c || !c->has_surface || !c->has_swapchain) { LOG("[RED] Vulkan: no device with a window surface (is the GPU driver installed? Qualcomm Adreno X1: Windows Update)"); return 0; }
    g_inst = c->inst; g_pd = c->pd; g_dev = c->dev; g_q = c->q; g_qf = c->qf;
    vk_probe();
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf};
    VKC(vkCreateCommandPool(g_dev, &pc, NULL, &g_cpool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKC(vkAllocateCommandBuffers(g_dev, &ca, &g_cb));
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT}; VKC(vkCreateFence(g_dev, &fc, NULL, &g_fence));
    VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(g_dev, &sc, NULL, &g_acq)); VKC(vkCreateSemaphore(g_dev, &sc, NULL, &g_done));
    return 1;
}
static void vk_window_down(void) {
    if (!g_dev) return;
    pthread_mutex_lock(&vkb()->qmu); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&vkb()->qmu);
    if (g_stg) { vkDestroyBuffer(g_dev, g_stg, NULL); vkFreeMemory(g_dev, g_stgm, NULL); g_stg = VK_NULL_HANDLE; g_stgp = NULL; }
    if (g_sc) { vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; }
}
static VkPresentModeKHR present_mode(void) {
    const char *e = getenv("DM_VSYNC");   // 0: as fast as the GPU goes (mailbox, else immediate); otherwise FIFO, the display's refresh
    if (!e || *e != '0') return VK_PRESENT_MODE_FIFO_KHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pm = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)dm_vk_instance_proc()(g_inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!pm) return VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0; pm(g_pd, g_surf, &n, NULL);
    VkPresentModeKHR m[16]; if (n > 16) n = 16; pm(g_pd, g_surf, &n, m);
    VkPresentModeKHR best = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) { if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR) return m[i]; if (m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) best = m[i]; }
    return best;
}
static int vk_window_up(void) {
    if (!vk_init()) return 0;
    if (!g_surf) {
        PFN_vkCreateWin32SurfaceKHR mk = (PFN_vkCreateWin32SurfaceKHR)dm_vk_instance_proc()(g_inst, "vkCreateWin32SurfaceKHR");
        if (!mk) { LOG("[RED] Vulkan: no vkCreateWin32SurfaceKHR"); return 0; }
        VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance = GetModuleHandleW(NULL), .hwnd = g_wnd};
        VKC(mk(g_inst, &si, NULL, &g_surf));
    }
    VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, g_surf, &ok);
    if (!ok) { LOG("[RED] Vulkan: the queue cannot present to the window"); return 0; }
    VkSurfaceCapabilitiesKHR cap; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &cap));
    if (!cap.currentExtent.width || !cap.currentExtent.height) return 0;   // minimized
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fm[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fm);
    g_fmt = fm[0].format; VkColorSpaceKHR cs = fm[0].colorSpace; g_sc_has_srgb = 0;
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_SRGB || fm[i].format == VK_FORMAT_R8G8B8A8_SRGB) g_sc_has_srgb = 1;
    for (uint32_t i = 0; i < nf; i++) {
        int srgb = fm[i].format == VK_FORMAT_B8G8R8A8_SRGB || fm[i].format == VK_FORMAT_R8G8B8A8_SRGB;
        int unorm = fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM;
        if ((g_sc_srgb && g_sc_has_srgb) ? srgb : unorm) { g_fmt = fm[i].format; cs = fm[i].colorSpace; break; }
    }
    g_ext2 = cap.currentExtent;
    uint32_t want = cap.minImageCount < 3 ? 3 : cap.minImageCount; if (cap.maxImageCount && want > cap.maxImageCount) want = cap.maxImageCount;
    VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surf, .minImageCount = want,
        .imageFormat = g_fmt, .imageColorSpace = cs, .imageExtent = g_ext2, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = cap.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = 1};
    VKC(vkCreateSwapchainKHR(g_dev, &sc, NULL, &g_sc));
    g_nimg = 8; VKC(vkGetSwapchainImagesKHR(g_dev, g_sc, &g_nimg, g_img));
    g_stg_sz = (VkDeviceSize)g_ext2.width * g_ext2.height * 4;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = g_stg_sz, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VKC(vkCreateBuffer(g_dev, &bc, NULL, &g_stg));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, g_stg, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    uint32_t mt = 0, need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (; mt < mp.memoryTypeCount; mt++) if ((mr.memoryTypeBits >> mt & 1) && (mp.memoryTypes[mt].propertyFlags & need) == need) break;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    VKC(vkAllocateMemory(g_dev, &ma, NULL, &g_stgm)); VKC(vkBindBufferMemory(g_dev, g_stg, g_stgm, 0)); VKC(vkMapMemory(g_dev, g_stgm, 0, g_stg_sz, 0, &g_stgp));
    LOG("window %ux%u, swapchain %u images, format %d, present mode %d", g_ext2.width, g_ext2.height, g_nimg, g_fmt, (int)sc.presentMode);
    return 1;
}
static void sc_follow_window(void) {
    if (!g_wnd || !g_dev) return;
    RECT r; GetClientRect(g_wnd, &r); int ww = r.right - r.left, wh = r.bottom - r.top;
    if (ww <= 0 || wh <= 0 || IsIconic(g_wnd)) return;
    if (g_sc && !g_sc_stale && (uint32_t)ww == g_ext2.width && (uint32_t)wh == g_ext2.height) return;
    g_sc_stale = 0; vk_window_down(); vk_window_up();
}
static int sc_encoding(VkFormat f) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_SC_SRGB"); on = !(e && *e == '0'); }
    int srgb = on && (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32);
    if (srgb == g_sc_srgb || (srgb && !g_sc_has_srgb)) return 0;
    LOG("[front] the frame is %s (format %d): swapchain made again", srgb ? "sRGB" : "not sRGB", (int)f);
    g_sc_srgb = srgb; g_sc_stale = 1; return 1;
}
// Copies the staging buffer (or blits a frame) into the next swapchain image and presents it
static int present_begin(uint32_t *idx) {
    vkWaitForFences(g_dev, 1, &g_fence, 1, UINT64_MAX);
    VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, 100000000ull, g_acq, VK_NULL_HANDLE, idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) g_sc_stale = 1;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 0;
    vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb, &cbi);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_img[*idx], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    return 1;
}
static void present_end(uint32_t idx) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_img[idx],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &g_cb,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &g_done};
    pthread_mutex_lock(&vkb()->qmu); vkQueueSubmit(g_q, 1, &si, g_fence);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_done, .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx};
    VkResult r = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&vkb()->qmu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_sc_stale = 1;
}

// ---------- Status picture: GDI text in a DIB, copied into the staging buffer ----------
static HDC g_sdc; static HBITMAP g_sbmp; static void *g_spx; static uint32_t g_sw, g_sh;
static void status_text(const char *s, int x, int y, int size, COLORREF c) {
    wchar_t w[512]; MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 512);
    HFONT f = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ o = SelectObject(g_sdc, f); SetTextColor(g_sdc, c); SetBkMode(g_sdc, TRANSPARENT);
    TextOutW(g_sdc, x, y, w, (int)wcslen(w)); SelectObject(g_sdc, o); DeleteObject(f);
}
static void status_text_c(const char *s, int cx, int y, int size, COLORREF c) {   // centred on cx
    wchar_t w[512]; MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 512);
    HFONT f = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ o = SelectObject(g_sdc, f); SIZE ts; GetTextExtentPoint32W(g_sdc, w, (int)wcslen(w), &ts); SetTextColor(g_sdc, c); SetBkMode(g_sdc, TRANSPARENT);
    TextOutW(g_sdc, cx - ts.cx / 2, y - ts.cy / 2, w, (int)wcslen(w)); SelectObject(g_sdc, o); DeleteObject(f);
}
static void status_draw(void) {
    if (!g_sc || !g_stgp) return;
    if (g_sw != g_ext2.width || g_sh != g_ext2.height) {
        if (g_sbmp) DeleteObject(g_sbmp);
        if (!g_sdc) g_sdc = CreateCompatibleDC(NULL);
        BITMAPINFO bi = {.bmiHeader = {sizeof(BITMAPINFOHEADER), (LONG)g_ext2.width, -(LONG)g_ext2.height, 1, 32, BI_RGB}};
        g_sbmp = CreateDIBSection(g_sdc, &bi, DIB_RGB_COLORS, &g_spx, NULL, 0); if (!g_sbmp) return;
        SelectObject(g_sdc, g_sbmp); g_sw = g_ext2.width; g_sh = g_ext2.height;
    }
    int st = atomic_load(&g_state);
    static const COLORREF bg[] = {RGB(0x20, 0x18, 0x10), RGB(0x20, 0x18, 0x10), RGB(0x1E, 0x3B, 0x0E), RGB(0x10, 0x10, 0x4A), RGB(0x20, 0x18, 0x10), RGB(0x20, 0x20, 0x20), RGB(0x10, 0x10, 0x4A), RGB(0x20, 0x18, 0x10)};
    RECT all = {0, 0, (LONG)g_sw, (LONG)g_sh}; HBRUSH br = CreateSolidBrush(bg[st]); FillRect(g_sdc, &all, br); DeleteObject(br);
    int u = (int)((g_sw < g_sh ? g_sw : g_sh) / 20); if (u < 8) u = 8; int y = u;
    char line[300];
    if (st == ST_START || st == ST_GAME) {   // the loader: the game's name, a spinner, the phase from the log, the time and what the GPU has compiled
        GdiFlush();   // the background's fill lands before the spinner's pixels
        status_text_c(DM_GAME_NAME, (int)g_sw / 2, (int)(g_sh * 0.22), (int)(1.5 * u), RGB(255, 255, 255));
        status_text_c(DM_GAME_ID " \u2014 nxnative, Windows on Arm", (int)g_sw / 2, (int)(g_sh * 0.22) + 2 * u, (int)(0.55 * u), RGB(0xA0, 0xA8, 0xB0));
        double t = (double)(dm_win_mono_ns() - g_t0) / 1e9; float cx = (float)g_sw / 2, cy = (float)g_sh * 0.5f;
        for (int k = 0; k < 12; k++) {
            double an = k * M_PI / 6 - M_PI / 2; float ph = (float)fmod(t * 1.1 - k / 12.0 + 12, 1.0);
            tp_disc(g_spx, (int)g_sw, (int)g_sh, cx + (float)(cos(an) * 1.3 * u), cy + (float)(sin(an) * 1.3 * u), (float)u * 0.16f, 0x8FD8FF, 0.15f + 0.85f * (1 - ph));
        }
        char ph[80]; pthread_mutex_lock(&g_txt_mu); snprintf(ph, sizeof ph, "%s", g_phase); pthread_mutex_unlock(&g_txt_mu);
        status_text_c(ph, (int)g_sw / 2, (int)(cy + 2.6 * u), (int)(0.85 * u), RGB(0xE8, 0xE8, 0xE8));
        uint64_t mods = 0, pipes = 0; if (st == ST_GAME) mtl_create_stats(&mods, &pipes);
        int sec = (int)t; snprintf(line, sizeof line, "%d:%02d", sec / 60, sec % 60);
        if (mods || pipes) snprintf(line + strlen(line), sizeof line - strlen(line), "  \u00b7  shaders %llu  \u00b7  pipelines %llu", (unsigned long long)mods, (unsigned long long)pipes);
        status_text_c(line, (int)g_sw / 2, (int)(cy + 3.8 * u), (int)(0.6 * u), RGB(0xB0, 0xB8, 0xC0));
        if (t > 20) status_text_c("The first start compiles the game's shaders and takes longer; later starts reuse them.", (int)g_sw / 2, (int)(cy + 4.9 * u), (int)(0.48 * u), RGB(0x80, 0xD0, 0xFF));
    } else {
        status_text("nxnative \u2014 Windows on Arm", u, y, (int)(1.4 * u), RGB(255, 255, 255)); y += 2 * u;
        snprintf(line, sizeof line, "%s (%s)", DM_GAME_NAME, DM_GAME_ID); status_text(line, u, y, (int)(0.7 * u), RGB(0xE0, 0xE0, 0xE0)); y += u;
        static const char *what[] = {"starting", "self test running", "self test PASSED", "self test FAILED", "game running", "host finished", "host crashed: see log.txt", "NVN picture check"};
        snprintf(line, sizeof line, "state: %s", what[st]); status_text(line, u, y, (int)(0.9 * u), RGB(0xE0, 0xE0, 0xE0)); y += (int)(1.3 * u);
        if (st == ST_SELFTEST || st == ST_PASS || st == ST_FAIL) { snprintf(line, sizeof line, "No game in %s", g_game); status_text(line, u, y, (int)(0.5 * u), RGB(0xA0, 0xA0, 0xA0)); }
    }
    pthread_mutex_lock(&g_txt_mu); snprintf(line, sizeof line, "%.150s", g_last); pthread_mutex_unlock(&g_txt_mu);
    status_text(line, u, (int)g_sh - (int)(1.2 * u), (int)(0.45 * u), RGB(0x90, 0x90, 0x90));
    GdiFlush();
    // the DIB is BGRA: swapped for an RGBA swapchain
    int swap = g_fmt == VK_FORMAT_R8G8B8A8_UNORM || g_fmt == VK_FORMAT_R8G8B8A8_SRGB;
    const uint8_t *s = g_spx; uint8_t *d = g_stgp; size_t n = (size_t)g_sw * g_sh;
    if (!swap) for (size_t i = 0; i < n; i++) { d[4 * i] = s[4 * i]; d[4 * i + 1] = s[4 * i + 1]; d[4 * i + 2] = s[4 * i + 2]; d[4 * i + 3] = 255; }
    else for (size_t i = 0; i < n; i++) { d[4 * i] = s[4 * i + 2]; d[4 * i + 1] = s[4 * i + 1]; d[4 * i + 2] = s[4 * i]; d[4 * i + 3] = 255; }
    uint32_t idx; if (!present_begin(&idx)) return;
    VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {g_ext2.width, g_ext2.height, 1}};
    vkCmdCopyBufferToImage(g_cb, g_stg, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    present_end(idx);
}

// ---------- The game's picture: its last presented window texture, scaled to fit (aspect kept), on black ----------
static uint64_t g_shown;
static int g_nearest, g_show_fps;   // settings: the picture's filter (sharp), the frame-rate note
static int frame_draw(void) {
    int st = atomic_load(&g_state);
    sc_follow_window();
    if (!g_sc || (st != ST_GAME && st != ST_DEMO)) return 0;
    vkb_frame fr; if (!vkb_frame_lock(&fr)) return 0;
    if (fr.n == g_shown) { vkb_frame_unlock(); return 1; }
    if (sc_encoding(fr.fmt)) { vkb_frame_unlock(); return 1; }
    uint32_t idx; if (!present_begin(&idx)) { vkb_frame_unlock(); return 1; }
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cb, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    int cw = fr.crop[2] > 0 ? fr.crop[2] : (int)fr.w, ch = fr.crop[3] > 0 ? fr.crop[3] : (int)fr.h;
    double sx = (double)g_ext2.width / cw, sy = (double)g_ext2.height / ch, sc = sx < sy ? sx : sy;
    int dw = (int)(cw * sc), dh = (int)(ch * sc), dx = ((int)g_ext2.width - dw) / 2, dy = ((int)g_ext2.height - dh) / 2;
    g_pic[0] = dx; g_pic[1] = dy; g_pic[2] = dw; g_pic[3] = dh; ovl_picture(g_pic);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{fr.crop[0], fr.crop[1], 0}, {fr.crop[0] + cw, fr.crop[1] + ch, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    if (fr.flip) { bl.srcOffsets[0].y = fr.crop[1] + ch; bl.srcOffsets[1].y = fr.crop[1]; }
    vkCmdBlitImage(g_cb, fr.img, VK_IMAGE_LAYOUT_GENERAL, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, g_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    present_end(idx);
    vkWaitForFences(g_dev, 1, &g_fence, 1, UINT64_MAX);
    g_shown = fr.n; vkb_frame_unlock();
    return 1;
}

// ---------- Input: XInput controllers and the keyboard, merged for player 1 as front_android.c merges its sources (buttons ORed, a tilted stick wins) ----------
static uint64_t g_key_btn; static int g_key_ax[4];   // the keyboard's buttons and its sticks' directions (-1, 0, 1)
static int g_xi_ok[4]; static uint64_t g_xi_btn[4]; static uint32_t g_xi_src[4]; static int32_t g_xi_ax[4][4];
static int tilted(int32_t x, int32_t y) { return (int64_t)x * x + (int64_t)y * y > (int64_t)4000 * 4000; }
// The controller's inputs as remap.h's sources (bit i = source i: a b x y lb rb lt rt ls rs start back up down left right), then mapped (the menu's Controls page)
static uint32_t xi_sources(const XINPUT_GAMEPAD *g) {
    static const WORD k[RM_NSRC] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
    uint32_t b = 0;
    for (int i = 0; i < RM_NSRC; i++) if (k[i] && (g->wButtons & k[i])) b |= 1u << i;
    if (g->bLeftTrigger > 100) b |= 1u << 6;
    if (g->bRightTrigger > 100) b |= 1u << 7;
    return b;
}
static int32_t xi_axis(SHORT v, SHORT dead) {
    int x = v; if (x > -dead && x < dead) return 0;
    if (x > 32767) x = 32767; if (x < -32767) x = -32767;
    return (int32_t)x;
}
static void publish(void) {
    int32_t k[4]; for (int i = 0; i < 4; i++) k[i] = g_key_ax[i] * DM_STICK_MAX;
    int32_t lx = k[0], ly = k[1], rx = k[2], ry = k[3];
    uint64_t b = g_key_btn | (g_xi_ok[0] ? g_xi_btn[0] : 0);
    if (g_xi_ok[0]) {
        if (tilted(g_xi_ax[0][0], g_xi_ax[0][1])) { lx = g_xi_ax[0][0]; ly = g_xi_ax[0][1]; }
        if (tilted(g_xi_ax[0][2], g_xi_ax[0][3])) { rx = g_xi_ax[0][2]; ry = g_xi_ax[0][3]; }
    }
    if (ovl_menu_open()) { dm_input_set(0, 0, 0, 0, 0); return; }   // the settings menu takes the input
    uint64_t tb; int32_t ta[4]; ovl_input(&tb, ta); b |= tb;   // the touch controls (overlay_windows.c): a tilted touch stick wins too
    if (tilted(ta[0], ta[1])) { lx = ta[0]; ly = ta[1]; }
    if (tilted(ta[2], ta[3])) { rx = ta[2]; ry = ta[3]; }
    dm_input_set(b, lx, ly, rx, ry);
}
// Rumble: the host's sink runs under its lock and must not call the hardware, so it only stores the motors' levels; the main loop applies them
static _Atomic uint32_t g_rumble[4];   // low motor << 16 | high motor, 0..65535 each
static void rumble_sink(dm_haptic_state s) {
    if (s.player < 0 || s.player > 3) return;
    float lo = s.motor[0].amplitude_low > s.motor[1].amplitude_low ? s.motor[0].amplitude_low : s.motor[1].amplitude_low;
    float hi = s.motor[0].amplitude_high > s.motor[1].amplitude_high ? s.motor[0].amplitude_high : s.motor[1].amplitude_high;
    if (lo > 1) lo = 1; if (hi > 1) hi = 1; if (lo < 0) lo = 0; if (hi < 0) hi = 0;
    atomic_store(&g_rumble[s.player], (uint32_t)(lo * 65535) << 16 | (uint32_t)(hi * 65535));
}
static void xinput_poll(void) {
    static uint64_t next_scan; static uint32_t applied[4]; uint64_t now = dm_win_mono_ns();
    uint32_t mask = 1;
    for (int i = 0; i < 4; i++) {
        if (!g_xi_ok[i] && now < next_scan) continue;   // XInputGetState on an empty slot is slow: those are tried twice a second
        XINPUT_STATE xs; DWORD r = XInputGetState((DWORD)i, &xs);
        int was = g_xi_ok[i]; g_xi_ok[i] = r == ERROR_SUCCESS;
        if (g_xi_ok[i] != was) { LOG("controller %d %s (XInput)", i + 1, g_xi_ok[i] ? "connected" : "disconnected"); dm_haptics_route(i, g_xi_ok[i]); applied[i] = ~0u; }
        if (!g_xi_ok[i]) continue;
        g_xi_src[i] = xi_sources(&xs.Gamepad); g_xi_btn[i] = rm_pad_buttons(ovl_map(), g_xi_src[i]);
        { static DWORD seen[4]; if (xs.dwPacketNumber != seen[i]) { if (seen[i] && (g_xi_btn[i] || tilted(xi_axis(xs.Gamepad.sThumbLX, 8000), xi_axis(xs.Gamepad.sThumbLY, 8000)))) ovl_other_input(); seen[i] = xs.dwPacketNumber; } }
        g_xi_ax[i][0] = xi_axis(xs.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE); g_xi_ax[i][1] = xi_axis(xs.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        g_xi_ax[i][2] = xi_axis(xs.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE); g_xi_ax[i][3] = xi_axis(xs.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        if (i) { dm_pad_set_player(i, g_xi_btn[i], g_xi_ax[i][0], g_xi_ax[i][1], g_xi_ax[i][2], g_xi_ax[i][3]); mask |= 1u << i; }
        uint32_t rv = atomic_load(&g_rumble[i]);
        { double k = st_num("rumble", 1); if (k < 0) k = 0; if (k > 1) k = 1;   // the menu's rumble: on / weak / off
          if (k < 1) rv = (uint32_t)((rv >> 16) * k) << 16 | (uint32_t)((rv & 0xffff) * k); }
        if (rv != applied[i]) { XINPUT_VIBRATION v = {(WORD)(rv >> 16), (WORD)(rv & 0xffff)}; XInputSetState((DWORD)i, &v); applied[i] = rv; }
    }
    if (now >= next_scan) next_scan = now + 500000000ull;
    { static uint32_t was; uint32_t b = g_xi_ok[0] ? g_xi_src[0] : 0; if (ovl_menu_open()) ovl_menu_pad(b & ~was); was = b; }   // the menu: the D-pad, A, B as pressed (not as mapped)
    dm_pad_players(mask);
    publish();
}
// The keyboard (the menu's Controls page maps it; Ryujinx's keys by default): its stick directions are held keys, -1 / 0 / 1 per axis
static int g_key_held[8];   // remap.h's RM_LUP.. order: left up down left right, right up down left right
static void key_axes(void) {
    const int *h = g_key_held;
    g_key_ax[0] = h[3] - h[2]; g_key_ax[1] = h[0] - h[1]; g_key_ax[2] = h[7] - h[6]; g_key_ax[3] = h[4] - h[5];
}
static int key_axis_set(WPARAM vk, int down) {
    int d[8], any = 0; rm_key_dirs(ovl_map(), (int)vk, d);
    for (int i = 0; i < 8; i++) if (d[i]) { g_key_held[i] = down; any = 1; }
    if (any) key_axes();
    return any;
}
static void keys_release(void) { g_key_btn = 0; memset(g_key_held, 0, sizeof g_key_held); key_axes(); }
static uint64_t key_button(WPARAM vk) { return rm_key_buttons(ovl_map(), (int)vk); }
// A long stall after the first frame (shaders compiling, a level loading): a note with a spinner over the picture, so a still picture does not look frozen
static void stall_note(void) {
    static uint64_t seen_n, seen_at; uint64_t now = dm_win_mono_ns();
    if (atomic_load(&g_state) != ST_GAME || !g_shown) { ovl_badge(NULL); seen_at = now; return; }
    if (g_shown != seen_n) { seen_n = g_shown; seen_at = now; ovl_badge(NULL); return; }
    double idle = (double)(now - seen_at) / 1e9;
    if (idle < st_num("stall_note", 3) || GetForegroundWindow() != g_wnd || st_num("stall_note", 3) <= 0) { ovl_badge(NULL); return; }
    uint64_t mods = 0, pipes = 0; mtl_create_stats(&mods, &pipes); char t[160]; double gw = mtl_gpu_wait_s();
    // the shader / pipeline counts are totals since the start: with the GPU on one submission for seconds (a hang Windows may yet reset), the note says so instead
    if (gw >= 2) snprintf(t, sizeof t, "Waiting for the GPU (%.0f s): it has not finished its work", gw);
    else snprintf(t, sizeof t, "Still working (%.0f s)  \u00b7  shaders %llu, pipelines %llu made so far", idle, (unsigned long long)mods, (unsigned long long)pipes);
    ovl_badge(t);
}

// ---------- Settings (settings.ini, the F2 menu): the ones the host reads at its start go into its environment; the others apply at once ----------
static const struct { const char *key, *env; } k_env[] = {
    {"fps_target", "DM_FPS_TARGET"}, {"opmode", "DM_OPMODE"}, {"render_scale", "DM_RENDER_SCALE"}, {"async_shaders", "DM_VK_ASYNC_PIPE"}, {"vsync", "DM_VSYNC"},
    {"vk_robust", "DM_VK_ROBUST"}, {"spv_loop_cap", "DM_SPV_LOOP_CAP"}, {"skip_shaders", "DM_VK_SKIP_SHADER"}, {"spv_demote", "DM_SPV_DEMOTE"},
};
static void settings_env(void) {
    for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; i++) {
        const char *k, *v; int found = 0;
        for (int j = 0; st_each(j, &k, &v); j++) if (!_stricmp(k, k_env[i].key)) { found = 1; if (*v) { setenv(k_env[i].env, v, 1); LOG("settings.ini: %s=%s (%s)", k, v, k_env[i].env); } }
        (void)found;
    }
    const char *f = st_str("filter", "linear"); g_nearest = !_stricmp(f, "nearest");
    g_show_fps = st_num("show_fps", 0) > 0;
}
void front_setting_changed(const char *key) {
    if (!strcmp(key, "vsync")) { setenv("DM_VSYNC", st_str("vsync", "1"), 1); g_sc_stale = 1; }
    else if (!strcmp(key, "filter")) g_nearest = !_stricmp(st_str("filter", "linear"), "nearest");
    else if (!strcmp(key, "rumble")) dm_haptics_set_enabled(st_num("rumble", 1) > 0);
    else if (!strcmp(key, "show_fps")) { g_show_fps = st_num("show_fps", 0) > 0; if (!g_show_fps) ovl_fps(NULL); }
}
static void close_game(void);
void front_restart(void) {   // the same program and arguments again, told to wait for this one to end (its saves written) before it starts
    wchar_t exe[MAX_PATH]; if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    static wchar_t cmd[4096]; swprintf(cmd, 4096, L"%ls --wait-pid %lu", GetCommandLineW(), (unsigned long)GetCurrentProcessId());
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { LOG("[RED] restart: CreateProcess failed (%lu)", (unsigned long)GetLastError()); return; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    LOG("settings: restarting the game to apply them");
    close_game();
}
static void fps_note(void) {   // frames shown in the last second
    static uint64_t at, n0, last_n; uint64_t now = dm_win_mono_ns();
    if (!g_show_fps) return;
    static uint64_t shown; if (g_shown && g_shown != last_n) { last_n = g_shown; shown++; }
    if (!at) { at = now; n0 = shown; return; }
    if (now - at < 1000000000ull) return;
    char t[48]; snprintf(t, sizeof t, "%.0f fps", (double)(shown - n0) * 1e9 / (double)(now - at)); ovl_fps(t);
    at = now; n0 = shown;
}

// ---------- The window ----------
static int g_full; static WINDOWPLACEMENT g_wp = {sizeof g_wp};
static void full_screen(int on) {
    DWORD st = (DWORD)GetWindowLongW(g_wnd, GWL_STYLE);
    if (on && !g_full) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(g_wnd, &g_wp) && GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(g_wnd, GWL_STYLE, (LONG)(st & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_full = 1;
        }
    } else if (!on && g_full) {
        SetWindowLongW(g_wnd, GWL_STYLE, (LONG)(st | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_wnd, &g_wp);
        SetWindowPos(g_wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full = 0;
    }
    g_sc_stale = 1;
}
// A lost GPU (gfx_vk.c gpu_fault_log: Windows' hang recovery reset it, or the driver refuses every submission): the replay can't go on, so instead of a frozen picture
// the player is asked whether to start the game again (its saves are on disk; the start record vk_start.txt keeps device_lost=). DM_GPU_LOST_NOASK=1: closed, no question
enum { WM_GPU_LOST = WM_APP + 7 };
static void gpu_lost_hook(int r) { LOG("[RED] the GPU is gone (%d): asking to restart the game", r); PostMessageW(g_wnd, WM_GPU_LOST, (WPARAM)r, 0); }
static void close_game(void);
static void gpu_lost(int r) {
    static int once; if (once++) return;
    const char *na = getenv("DM_GPU_LOST_NOASK");
    if (na && *na == '1') { LOG("the GPU is gone (%d): the game closes (DM_GPU_LOST_NOASK)", r); close_game(); return; }
    if (g_full) full_screen(0);
    wchar_t m[600]; swprintf(m, 600, L"The graphics driver stopped the GPU after it hung (Vulkan %d). The game can't go on in this window.\n\n"
        L"Start it again now? What you saved in the game is kept.\n\nlog.txt beside dmhost.exe says which draws the GPU was on: please send it.", r);
    int a = MessageBoxW(g_wnd, m, L"nxnative: the GPU stopped", MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND);
    LOG("the GPU is gone (%d): %s", r, a == IDYES ? "restarting the game" : "closing");
    if (a == IDYES) front_restart(); else close_game();
}
static DWORD WINAPI pc_save_thread(void *u) { (void)u; vkb_pipeline_cache_save(); return 0; }   // off the window's thread: a big cache takes a moment
static void close_game(void) {
    if (atomic_exchange(&g_closing, 1)) return;
    LOG("window closed: the game ends as on SIGTERM (its saves are kept)");
    vkb_pipeline_cache_save();
    if (atomic_load(&g_state) == ST_GAME) {
        dm_win_post_term();   // hle_lifecycle.c's waiter: dm_host_shutdown, which exits the process
        // the host's own deadline is 1.5 s; this is the window's
        ShowWindow(g_wnd, SW_HIDE);
        Sleep(4000);
    }
    vk_window_down();
    ExitProcess(0);
}
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    { LRESULT r; if (ovl_wndproc(h, m, w, l, &r)) { publish(); return r; } }   // touch, pen and the mouse's left button: the touch controls or the game's touch screen
    int game = atomic_load(&g_state) == ST_GAME;
    switch (m) {
    case WM_CLOSE: close_game(); return 0;
    case WM_GPU_LOST: gpu_lost((int)w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE: g_sc_stale = 1;
        if (game) { static int min; int now = w == SIZE_MINIMIZED; if (now != min) { min = now; dm_focus_set(now ? 3 : 1); dm_audio_suspend(now);
                  if (now) { HANDLE t = CreateThread(NULL, 0, pc_save_thread, NULL, 0, NULL); if (t) CloseHandle(t); } } }   // minimized: the pipelines kept now, as Android does on a pause
        return 0;
    case WM_ACTIVATEAPP:
        if (!w) { keys_release(); publish(); }
        if (game) dm_focus_set(w ? 1 : 2);
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (HIWORD(l) & KF_ALTDOWN)) { full_screen(!g_full); return 0; }
        break;
    case WM_KEYDOWN: case WM_KEYUP: {
        int down = m == WM_KEYDOWN;
        if (down && w == VK_F11) { full_screen(!g_full); return 0; }
        if (down && w == VK_F1) { ovl_toggle(); publish(); return 0; }
        if (down && w == VK_F2 && !ovl_menu_open()) { keys_release(); ovl_menu(1); publish(); return 0; }
        if (ovl_menu_open()) { if (down) ovl_menu_key(w); if (!ovl_menu_open()) keys_release(); publish(); return 0; }
        if (down && w == VK_ESCAPE && g_full) { full_screen(0); return 0; }
        if (key_axis_set(w, down)) { if (down) ovl_other_input(); publish(); return 0; }
        uint64_t b = key_button(w);
        if (b) { if (down) { g_key_btn |= b; ovl_other_input(); } else g_key_btn &= ~b; publish(); return 0; }
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); g_shown = 0; return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------- Start ----------
static void utf8_of(const wchar_t *w, char *out, size_t n) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
}
extern void (*dm_win_fatal_hook)(const char *what);   // posix_windows.c
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    (void)prev; (void)cmd;
    // First the log, before anything that can fail: log.txt beside the program (the previous one kept as log.prev.txt)
    wchar_t wp[MAX_PATH]; GetModuleFileNameW(NULL, wp, MAX_PATH); wchar_t *bs = wcsrchr(wp, L'\\'); if (bs) *bs = 0;
    utf8_of(wp, g_dir, sizeof g_dir);
    char lp[700]; snprintf(lp, sizeof lp, "%s/log.txt", g_dir); keep_prev(lp);
    _snwprintf(g_logw, MAX_PATH + 15, L"%ls\\log.txt", wp);
    int logged = log_open();
    { char cp[700]; snprintf(cp, sizeof cp, "%s/crash.txt", g_dir); keep_prev(cp); }   // a crash.txt is this start's only: an older one becomes crash.prev.txt
    dm_win_fatal_hook = fatal_note;
    { void dm_win_watch_start(const wchar_t *log_path); if (logged) dm_win_watch_start(g_logw); }   // watch_windows.c: the threads' states in the log when it goes quiet (DM_WIN_WATCH)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int console = 0; const char *game_arg = NULL; static char ga[700];
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--console")) console = 1;
        else if (!wcscmp(argv[i], L"--game") && i + 1 < argc) { utf8_of(argv[++i], ga, sizeof ga); game_arg = ga; }
        else if (!wcscmp(argv[i], L"--selftest")) setenv("DM_FORCE_SELFTEST", "1", 1);
    }
    snprintf(g_game, sizeof g_game, "%s/game", g_dir);
    {   // %LOCALAPPDATA%\nxnative\<title ID>
        PWSTR la = NULL; char l8[MAX_PATH * 3] = "";
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la))) { utf8_of(la, l8, sizeof l8); CoTaskMemFree(la); }
        snprintf(g_data, sizeof g_data, "%s/nxnative/%s", l8[0] ? l8 : g_dir, DM_GAME_ID); mkdirs(g_data);
    }
    st_load(g_data); g_t0 = dm_win_mono_ns();
    for (int i = 1; i + 1 < argc; i++) if (!wcscmp(argv[i], L"--wait-pid")) {   // a restart from the settings menu: the earlier run ends first
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(argv[i + 1], NULL, 10)); if (ph) { WaitForSingleObject(ph, 15000); CloseHandle(ph); }
    }
    log_start(console);
    if (!logged) LOG("[RED] cannot write %s: is the folder read-only? (the log goes to the console only)", lp);
    LOG("nxnative on Windows on Arm: %s (%s), program folder %s, game %s, data %s", DM_GAME_NAME, DM_GAME_ID, g_dir, game_arg ? game_arg : g_game, g_data);
    env_file(g_dir, "env.txt", 1); env_file(game_arg ? game_arg : g_game, "launcher.txt", 0); env_file(game_arg ? game_arg : g_game, "defaults.txt", 0);
    // a game's defaults built into the host, for a game folder made before its profile's windows_defaults.txt had them (env.txt or defaults.txt still win).
    // Pokémon Pokopia: its 3D scene is composed into a 1920x1080 RGBA8 sRGB image through an image store, which the Adreno X1-45 cannot store to in sRGB: skipped, the
    // world drew black under the UI. DM_VK_STORAGE_UP=2 stores it through its UNORM twin (the texels as written, as Maxwell's stores)
    // and robust access: on the Adreno X1-45 its world's draws (vertex 85c01ed67ec089e9, fragment 3fb06ea7d8ed1272) hung the GPU 100-130 s in, without it, in every run
    // (until Windows reset it, the device lost); with it (and the loop cap) a run went through to frame 5,700. DM_VK_ROBUST=0 (env.txt) or the menu turns it off
    static const struct { const char *game, *key, *val; } k_game_env[] = {{"0400A3A01EE4C000", "DM_VK_STORAGE_UP", "2"}, {"0400A3A01EE4C000", "DM_VK_ROBUST", "1"}};
    for (size_t i = 0; i < sizeof k_game_env / sizeof k_game_env[0]; i++)
        if (!strcmp(DM_GAME_ID, k_game_env[i].game) && !getenv(k_game_env[i].key)) { setenv(k_game_env[i].key, k_game_env[i].val, 1); LOG("game default: %s=%s", k_game_env[i].key, k_game_env[i].val); }
    settings_env();
    vk_driver_dirs(g_dir, g_data);
    vk_driver_game(game_arg ? game_arg : g_game, DM_GAME_ID);
    vk_start_game(g_data);
    dm_haptics_init(rumble_sink, st_num("rumble", 1) > 0);   // settings.ini rumble (or DM_RUMBLE): 1 on, 0.5 weak, 0 off; the menu changes it live

    WNDCLASSEXW wc = {sizeof wc, CS_OWNDC, wnd_proc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL, NULL, L"nxnative", NULL};
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    RegisterClassExW(&wc);
    wchar_t title[200]; { wchar_t gn[160]; MultiByteToWideChar(CP_UTF8, 0, DM_GAME_NAME, -1, gn, 160); swprintf(title, 200, L"%ls \u2014 nxnative", gn); }
    RECT r = {0, 0, 1280, 720};
    UINT dpi = GetDpiForSystem(); r.right = MulDiv(r.right, (int)dpi, 96); r.bottom = MulDiv(r.bottom, (int)dpi, 96);
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    g_wnd = CreateWindowExW(0, L"nxnative", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!g_wnd) { LOG("[RED] cannot make the window (error %lu)", (unsigned long)GetLastError()); return 1; }
    ShowWindow(g_wnd, show == SW_HIDE ? SW_SHOWNORMAL : show); UpdateWindow(g_wnd);
    { const char *fs = getenv("DM_FULLSCREEN"); if (fs && atoi(fs)) full_screen(1); }
    ovl_init(g_wnd);
    vk_lost_hook = gpu_lost_hook;
    if (!vk_window_up()) LOG("[RED] no picture: the window's Vulkan swapchain failed (the lines above say why); the game still runs");
    start_host(game_arg);
    for (;;) {
        MSG msg;
        int game = atomic_load(&g_state) == ST_GAME || atomic_load(&g_state) == ST_DEMO;
        DWORD wait = g_sc ? (game ? 2 : 100) : 100;
        MsgWaitForMultipleObjectsEx(0, NULL, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { close_game(); return 0; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        xinput_poll();
        if (!frame_draw()) { static uint64_t last; uint64_t now = dm_win_mono_ns(); if (now - last > 33000000ull) { last = now; status_draw(); } }   // the loader, 30 times a second
        stall_note(); fps_note();
        { static double at = -1; static int step; if (at < 0) { const char *e = getenv("DM_OPEN_MENU_AFTER"); at = e ? atof(e) : 0; }   // CI: the menu's pictures
          double t = (double)(dm_win_mono_ns() - g_t0) / 1e9;
          if (at > 0 && step == 0 && t > at) { step = 1; ovl_menu(1); }
          else if (at > 0 && step == 1 && t > at + 3) { step = 2; for (int k = 0; k < 3; k++) ovl_menu_key(VK_UP); ovl_menu_key(VK_RETURN); } }   // then its Controls page, by the keys
        { static double at = -1; if (at < 0) { const char *e = getenv("DM_TEST_GPU_LOST"); at = e ? atof(e) : 0; }   // CI: the lost-GPU path, as if the driver had reset it after N s
          if (at > 0 && (double)(dm_win_mono_ns() - g_t0) / 1e9 > at) { at = 0; gpu_lost_hook(-4); } }
        ovl_follow();
        vk_start_tick();
    }
}
