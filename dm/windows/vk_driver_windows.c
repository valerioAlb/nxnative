// The Vulkan driver behind gfx_vk.h's vk* pointers on Windows: the system's Vulkan loader, vulkan-1.dll (installed with every GPU driver: on a Snapdragon X, Qualcomm's
// Adreno X1 driver, which Windows Update keeps current). DM_VK_DRIVER=<file.dll>: another loader or driver ICD's vulkan-1.dll (a newer Vulkan Runtime, or Mesa's
// Dozen over Direct3D 12 for a test: the replay's mesh-shader and descriptor needs are only met by the native driver). The Android port's driver records and the crash guard
// are not here: a player does not swap the GPU driver per game on Windows.
#include <windows.h>
#include "dm.h"
#include "gfx_vk.h"
#include <stdatomic.h>

#define VKB_DEF(f) PFN_##f f;
VKB_FNS_GLOBAL(VKB_DEF) VKB_FNS(VKB_DEF)
static PFN_vkGetInstanceProcAddr g_gipa;
static char g_name[260];
void vk_driver_dirs(const char *ext, const char *internal) { (void)ext; (void)internal; }
const char *vk_driver_name(void) { return g_name[0] ? g_name : NULL; }
void vk_driver_game(const char *dir, const char *id) { (void)dir; (void)id; }
const char *vk_driver_note(int *warn) { if (warn) *warn = 0; return NULL; }
void vk_driver_ok(void) {}
void vk_driver_lost(VkResult r) { LOG("[RED] [vk] device lost (%s): the GPU driver reset; Adreno X1 drivers older than 31.0.x are known to lose the device on large mesh shaders", vk_result_str(r)); }
PFN_vkGetInstanceProcAddr dm_vk_instance_proc(void) { return g_gipa; }

static HMODULE open_dll(const char *path) {
    wchar_t w[1024]; if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0) return NULL;
    for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
    HMODULE m = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    return m ? m : LoadLibraryW(w);
}
#define VKB_LOAD_GLOBAL(f) f = (PFN_##f)g_gipa(NULL, #f);
int vk_driver_load(void) {
    if (g_gipa) return 1;
    const char *drv = getenv("DM_VK_DRIVER");
    HMODULE m = NULL;
    if (drv && *drv) {
        if ((m = open_dll(drv))) { snprintf(g_name, sizeof g_name, "%s", drv); LOG("[vk] DM_VK_DRIVER %s: running it", drv); }
        else LOG("[RED] [vk] DM_VK_DRIVER %s: cannot open it (error %lu): the system's Vulkan instead", drv, (unsigned long)GetLastError());
    }
    if (!m) m = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) m = LoadLibraryW(L"vulkan-1.dll");
    if (!m) { LOG("[RED] [vk] no vulkan-1.dll: install the GPU driver (Qualcomm Adreno X1: Windows Update, or the Surface's driver pack)"); return 0; }
    g_gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vkGetInstanceProcAddr");
    if (!g_gipa) { LOG("[RED] [vk] vulkan-1.dll has no vkGetInstanceProcAddr"); return 0; }
    VKB_FNS_GLOBAL(VKB_LOAD_GLOBAL)
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
}
int vk_driver_fail(const char *step) {
    const char *e = getenv("DM_VK_DRIVER_FAIL");
    return g_name[0] && e && !strcmp(e, step);
}
int vk_driver_fallback(const char *why) {
    if (!g_name[0]) return 0;
    LOG("[RED] [vk] DM_VK_DRIVER %s: %s; the system's Vulkan instead", g_name, why);
    g_name[0] = 0; g_gipa = NULL;
    setenv("DM_VK_DRIVER", "", 1);
    return vk_driver_load();
}

const char *vk_result_str(VkResult r) {
    switch (r) {
#define VKR(x) case x: return #x;
        VKR(VK_SUCCESS) VKR(VK_INCOMPLETE) VKR(VK_ERROR_OUT_OF_HOST_MEMORY) VKR(VK_ERROR_OUT_OF_DEVICE_MEMORY) VKR(VK_ERROR_INITIALIZATION_FAILED) VKR(VK_ERROR_DEVICE_LOST)
        VKR(VK_ERROR_LAYER_NOT_PRESENT) VKR(VK_ERROR_EXTENSION_NOT_PRESENT) VKR(VK_ERROR_FEATURE_NOT_PRESENT) VKR(VK_ERROR_INCOMPATIBLE_DRIVER) VKR(VK_ERROR_TOO_MANY_OBJECTS)
        VKR(VK_ERROR_FORMAT_NOT_SUPPORTED) VKR(VK_ERROR_UNKNOWN) VKR(VK_ERROR_SURFACE_LOST_KHR) VKR(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) VKR(VK_ERROR_OUT_OF_DATE_KHR)
#undef VKR
        default: { static _Thread_local char b[24]; snprintf(b, sizeof b, "VkResult %d", (int)r); return b; }
    }
}

// The floating-point environment of the driver's compiler. The replay and its pipeline threads run on threads the game's NVN calls started, or on the game's own, so
// they carry the game's FPCR: 0x1000000 (flush-to-zero) for Ocarina of Time. Qualcomm's compiler, run on the CPU inside vkCreate*Pipelines (Samsung's in
// vkCreateShaderModule), then folds constants with denormals flushed and makes different code: Swap2-Native measured Ocarina of Time's scene about 2^12 too bright
// at the bloom pass on driver 891.11, and BananaNative sets FPCR 0 around these calls (vka_fpenv.h). So the calls that compile (device, module, pipelines, and a
// submit, where a driver may compile what it deferred) run with FPCR 0 and give the caller's back after; the game's own threads keep theirs. The pipeline cache's
// file is named apart (gfx_vk_draw.inc pcache), so pipelines compiled under flush-to-zero are not taken from it. DM_VK_FP_HOST=0 (env.txt): the caller's FPCR, as before
#if defined(__aarch64__)
static int fp_host_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_FP_HOST"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_FP_HOST=0: the driver compiles with the calling thread's FPCR"); } return v; }
static inline uint64_t fp_enter(void) {
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    if (v && fp_host_on()) {
        static _Atomic int told; if (!atomic_exchange(&told, 1)) LOG("[vk] the calling thread's FPCR is %#llx: the driver compiles with FPCR 0 (DM_VK_FP_HOST=0: as it is)", (unsigned long long)v);
        __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)0)); return v;
    }
    return 0;
}
static inline void fp_leave(uint64_t v) { if (v) __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
#else
static inline uint64_t fp_enter(void) { return 0; }
static inline void fp_leave(uint64_t v) { (void)v; }
#endif
int vk_fp_host(void) {   // 1: compiles run with FPCR 0 (the pipeline cache's file is named for it)
#if defined(__aarch64__)
    return fp_host_on();
#else
    return 0;
#endif
}
static PFN_vkCreateDevice d_create_device; static PFN_vkCreateShaderModule d_create_module; static PFN_vkCreateGraphicsPipelines d_create_gp;
static PFN_vkCreateComputePipelines d_create_cp; static PFN_vkQueueSubmit d_submit;
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    uint64_t f = fp_enter(); VkResult r = d_create_device(pd, ci, a, d); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_module(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    uint64_t f = fp_enter(); VkResult r = d_create_module(d, ci, a, m); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_gp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t f = fp_enter(); VkResult r = d_create_gp(d, c, n, ci, a, p); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_cp(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t f = fp_enter(); VkResult r = d_create_cp(d, c, n, ci, a, p); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_submit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fe) {
    uint64_t f = fp_enter(); VkResult r = d_submit(q, n, si, fe); fp_leave(f); return r; }

void vk_driver_instance(VkInstance inst) {
#define VKB_LOAD(f) f = (PFN_##f)g_gipa(inst, #f);
    VKB_FNS(VKB_LOAD)
    if (vkCreateDevice) { d_create_device = vkCreateDevice; vkCreateDevice = fp_create_device; }   // through the FPCR guard (above)
    if (vkCreateShaderModule) { d_create_module = vkCreateShaderModule; vkCreateShaderModule = fp_create_module; }
    if (vkCreateGraphicsPipelines) { d_create_gp = vkCreateGraphicsPipelines; vkCreateGraphicsPipelines = fp_create_gp; }
    if (vkCreateComputePipelines) { d_create_cp = vkCreateComputePipelines; vkCreateComputePipelines = fp_create_cp; }
    if (vkQueueSubmit) { d_submit = vkQueueSubmit; vkQueueSubmit = fp_submit; }
}
