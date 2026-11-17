// Windows diagnostics for a start that stops with nothing in the log (Pokémon Pokopia on a Surface went quiet after its first Vulkan lines):
//   dm_win_dump_threads  every thread's pc / lr / sp and a few frames, as module+offset (the game's modules too), written straight to the log file with
//                        WriteFile: no CRT lock and no heap is taken while a thread is held, since the thread that hangs may hold them
//   the watchdog         (front_windows.c starts it) dumps the threads when the log has been silent for DM_WIN_WATCH seconds (default 10; 0: never), again
//                        after each further minute of silence, up to 6 times; it also puts the host's unhandled-exception filter back if a DLL replaced it
#include <windows.h>
#include <tlhelp32.h>
#include "dm.h"
#include <stdatomic.h>

static HANDLE g_out = INVALID_HANDLE_VALUE;   // the log file (appending), else stderr's handle
static void out_raw(const char *s, int n) { DWORD w; HANDLE h = g_out != INVALID_HANDLE_VALUE ? g_out : GetStdHandle(STD_ERROR_HANDLE); if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s, (DWORD)n, &w, NULL); }
static void outf(const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (n > (int)sizeof b - 1) n = sizeof b - 1; if (n > 0) out_raw(b, n); }

enum { MAXT = 512, MAXM = 512, NFR = 6 };
typedef struct { DWORD tid; int ok; DWORD64 pc, lr, sp, fp, fr[NFR]; } tstate;
typedef struct { uintptr_t lo, hi; char name[64]; } mstate;
static tstate g_t[MAXT]; static mstate g_m[MAXM]; static int g_nt, g_nm;
static SRWLOCK g_dl = SRWLOCK_INIT;

static void where(uint64_t a, char *b, size_t n) {
    for (int i = 0; i < g_nmod; i++) { uint64_t lo = (uint64_t)(uintptr_t)g_mod[i].base; if (g_mod[i].base && a >= lo && a < lo + g_mod[i].size) { snprintf(b, n, "%s+%#llx", g_mod[i].name, (unsigned long long)(a - lo)); return; } }
    for (int i = 0; i < g_nm; i++) if (a >= g_m[i].lo && a < g_m[i].hi) { snprintf(b, n, "%s+%#llx", g_m[i].name, (unsigned long long)(a - g_m[i].lo)); return; }
    snprintf(b, n, "%#llx", (unsigned long long)a);
}
void dm_win_dump_threads(const char *why) {
    if (!TryAcquireSRWLockExclusive(&g_dl)) return;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    g_nt = g_nm = 0;
    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);   // reads the process's module list without its loader lock
    if (ms != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me32 = {.dwSize = sizeof me32};
        for (BOOL k = Module32FirstW(ms, &me32); k && g_nm < MAXM; k = Module32NextW(ms, &me32)) {
            g_m[g_nm].lo = (uintptr_t)me32.modBaseAddr; g_m[g_nm].hi = g_m[g_nm].lo + me32.modBaseSize;
            WideCharToMultiByte(CP_UTF8, 0, me32.szModule, -1, g_m[g_nm].name, sizeof g_m[g_nm].name, NULL, NULL); g_nm++;
        }
        CloseHandle(ms);
    }
    HANDLE ts = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (ts != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = {.dwSize = sizeof te};
        for (BOOL k = Thread32First(ts, &te); k && g_nt < MAXT; k = Thread32Next(ts, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            tstate *t = &g_t[g_nt++]; memset(t, 0, sizeof *t); t->tid = te.th32ThreadID;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t->tid); if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {   // held only for the context: nothing here takes a lock the thread may hold
                CONTEXT c = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
                if (GetThreadContext(h, &c)) { t->ok = 1; t->pc = c.Pc; t->lr = c.Lr; t->sp = c.Sp; t->fp = c.Fp; }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        CloseHandle(ts);
    }
    for (int i = 0; i < g_nt; i++) {   // frame records {fp, lr} from the stack, read safely (a thread runs on meanwhile)
        DWORD64 fp = g_t[i].fp;
        for (int f = 0; f < NFR && fp && !(fp & 7); f++) { DWORD64 rec[2]; SIZE_T got = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)fp, rec, sizeof rec, &got) || got != sizeof rec) break; g_t[i].fr[f] = rec[1]; if (rec[0] <= fp) break; fp = rec[0]; }
    }
    outf("[watch] threads (%s): %d, this one %lu\n", why, g_nt, me);
    for (int i = 0; i < g_nt; i++) {
        tstate *t = &g_t[i]; if (!t->ok) { outf("[watch]   %lu: no context\n", t->tid); continue; }
        char a[96], b[96]; where(t->pc, a, sizeof a); where(t->lr, b, sizeof b);
        char fr[700] = ""; int n = 0;
        for (int f = 0; f < NFR && t->fr[f]; f++) { char c[96]; where(t->fr[f], c, sizeof c); n += snprintf(fr + n, sizeof fr - (size_t)n, " %s", c); if (n >= (int)sizeof fr - 1) break; }
        outf("[watch]   %lu: pc %s lr %s sp %#llx;%s\n", t->tid, a, b, (unsigned long long)t->sp, fr);
    }
    char nl[2048]; int n = snprintf(nl, sizeof nl, "[watch]   names:");   // last: GetThreadDescription allocates
    for (int i = 0; i < g_nt && n < (int)sizeof nl - 80; i++) {
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_t[i].tid); if (!h) continue;
        PWSTR d = NULL;
        if (SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0]) { char u[64]; WideCharToMultiByte(CP_UTF8, 0, d, -1, u, sizeof u, NULL, NULL); u[63] = 0; n += snprintf(nl + n, sizeof nl - (size_t)n, " %lu=%s", g_t[i].tid, u); }
        if (d) LocalFree(d);
        CloseHandle(h);
    }
    if (n < (int)sizeof nl - 1) nl[n++] = '\n'; out_raw(nl, n);
    ReleaseSRWLockExclusive(&g_dl);
}

void *dm_win_unhandled_filter(void);   // posix_windows.c
static uint64_t log_size(void) { LARGE_INTEGER s; return g_out != INVALID_HANDLE_VALUE && GetFileSizeEx(g_out, &s) ? (uint64_t)s.QuadPart : 0; }
static DWORD WINAPI watch_main(void *p) {
    int quiet = (int)(intptr_t)p, dumps = 0;
    SetThreadDescription(GetCurrentThread(), L"dm watch");
    uint64_t last = log_size(); ULONGLONG since = GetTickCount64(), next = (ULONGLONG)quiet * 1000;
    for (;;) {
        Sleep(1000);
        void *f = dm_win_unhandled_filter(), *was = (void *)SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)f);
        if (was != f) outf("[watch] the unhandled-exception filter had been replaced (by %p): the host's put back\n", was);
        uint64_t s = log_size(); ULONGLONG now = GetTickCount64();
        if (s != last) { last = s; since = now; next = (ULONGLONG)quiet * 1000; continue; }
        if (dumps < 6 && now - since >= next) {
            char w[80]; snprintf(w, sizeof w, "no log line for %llu s", (unsigned long long)((now - since) / 1000));
            dm_win_dump_threads(w); dumps++; next += 60000; last = log_size();
        }
    }
    return 0;
}
void dm_win_watch_start(const wchar_t *log_path) {   // the log file to append to and watch; the watchdog unless DM_WIN_WATCH=0
    if (log_path) g_out = CreateFileW(log_path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    const char *e = getenv("DM_WIN_WATCH"); int quiet = e && *e ? atoi(e) : 10;
    if (quiet <= 0 || g_out == INVALID_HANDLE_VALUE) return;
    HANDLE h = CreateThread(NULL, 256 << 10, watch_main, (void *)(intptr_t)quiet, 0, NULL); if (h) CloseHandle(h);
}
