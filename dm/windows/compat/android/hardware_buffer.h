// The Android port's AHardwareBuffer route for raw global memory (dm/android/gfx_vk_gmem.inc) has no Windows counterpart: the device offers no
// VK_ANDROID_external_memory_android_hardware_buffer, so the route is never taken; these let the shared file build, and every call fails
#pragma once
#include "dm_windows.h"
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_BLOB 0x21
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3ull
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN (3ull << 4)
#define AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER (1ull << 24)
#define AHARDWAREBUFFER_USAGE_SENSOR_DIRECT_DATA (1ull << 23)
static inline int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **b) { (void)d; *b = NULL; return -1; }
static inline void AHardwareBuffer_release(AHardwareBuffer *b) { (void)b; }
static inline int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *b, int fd) { (void)b; (void)fd; return -1; }
static inline void AHardwareBuffer_describe(const AHardwareBuffer *b, AHardwareBuffer_Desc *d) { (void)b; memset(d, 0, sizeof *d); }
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>   // the AHardwareBuffer import structures (dm/windows/compat/vulkan: Khronos's header)
