# Vulkan headers for the Windows build

`vulkan.h`, `vulkan_win32.h` and `vulkan_android.h` from Khronos's Vulkan-Headers, version 1.4.363 (the version of `dm/third_party/vulkan-headers`), unmodified.
They are kept here, not beside `vulkan_core.h`, so that the Android build keeps the NDK's `vulkan.h`. The Windows build puts
`dm/third_party/vulkan-headers/include/vulkan` on its include path for the `vulkan_core.h` and `vk_platform.h` they include.
`vulkan_android.h` only gives the shared Vulkan replay's AHardwareBuffer route (`dm/android/gfx_vk_gmem.inc`) its types; that route is never taken on Windows.
License: Apache-2.0 or MIT (`dm/third_party/vulkan-headers/LICENSE.md`).
