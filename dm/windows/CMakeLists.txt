# Windows on Arm build of the host (arm64, Snapdragon X / 8cx): the shared host sources (dm/host, the C parts of dm/gfx) with the Darwin and POSIX stand-ins of
# compat/, the Android port's Vulkan NVN replay (dm/android/gfx_vk.c) on a Win32 surface, WASAPI audio and XInput / keyboard input. Products, from one set of objects:
#   dmhost.exe       the game's host with a window (front_windows.c): one per game profile, as the Mac host is built per profile
#   dmhost_cli.exe   the same host without a window (front_cli.c), for SSH sessions and CI
#   woa_spike.exe    the platform checks (tests/woa_spike.c): thread slots through x18, fixed-address memory, faults caught and resumed, thread suspension
# Configure with llvm-mingw (cmake/aarch64-w64-mingw32.cmake) from any host, or natively on the device with llvm-mingw's arm64 release:
#   cmake -G Ninja -S dm/windows -B <build> -DCMAKE_TOOLCHAIN_FILE=dm/windows/cmake/aarch64-w64-mingw32.cmake -DLLVM_MINGW=<llvm-mingw> -DGAME=<base game title ID>
#         [-DGLSLANG=<glslang or glslangValidator>] [-DDM_TUNE=oryon-1] [-DDM_CPU=<cpu>] [-DDM_GAMES_DIR=<profiles folder outside dm/games>] [-DDM_GFX=vk|null] [-DDM_TESTS=ON]
cmake_minimum_required(VERSION 3.22)
project(dmhost_windows C CXX ASM RC)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
  message(FATAL_ERROR "dm/windows builds for Windows on Arm (arm64) only; the toolchain targets ${CMAKE_SYSTEM_PROCESSOR}: use cmake/aarch64-w64-mingw32.cmake")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Code generation for the CPU: DM_TUNE schedules for it (-mtune, no instruction the armv8.2-a baseline lacks: the .exe runs on any Windows on Arm PC; default oryon-1,
# Snapdragon X's cores); DM_CPU (-mcpu, empty by default) also uses its newer instructions: oryon-1 for a host that runs only on Snapdragon X
set(DM_TUNE "oryon-1" CACHE STRING "-mtune for the host (empty: the compiler's default)")
set(DM_CPU "" CACHE STRING "-mcpu for the host (empty: none, the armv8.2-a+lse baseline)")
if(DM_CPU)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${DM_CPU}>)
elseif(DM_TUNE)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mtune=${DM_TUNE}>)
endif()
set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(AND ${DM}/android)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN}/asm ${GEN}/shaders)

option(DM_SPIKE_ONLY "build only the platform checks (woa_spike.exe), no game profile needed" OFF)
# The manifest (dmhost.manifest: UTF-8 code page, long paths, DPI awareness) in the programs' resources (woa_spike's too: its heap checks run on the same segment heap)
set(DM_RC ${GEN}/dmhost.rc)
file(WRITE ${DM_RC} "1 24 \"${CMAKE_CURRENT_SOURCE_DIR}/dmhost.manifest\"\n")
set_source_files_properties(${DM_RC} PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dmhost.manifest)
add_executable(woa_spike tests/woa_spike.c plat_slots.c aligned_windows.c ${DM_RC})
target_include_directories(woa_spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(woa_spike PRIVATE -O1 -g -Wall -ffixed-x18)
if(DM_SPIKE_ONLY)
  return()
endif()

if(NOT GAME)
  message(FATAL_ERROR "GAME is not set: -DGAME=<base game title ID> (dm/games/<id>), or -DDM_SPIKE_ONLY=ON for the platform checks alone")
endif()
file(MAKE_DIRECTORY ${GEN}/${GAME})

# The profile header (dm/games/game.py header), regenerated at every configure and build; game.py writes it only when it changes
set(GAME_H ${GEN}/${GAME}/game_profile.h)
set(GAME_ENV)
if(DM_GAMES_DIR)
  set(GAME_ENV ${CMAKE_COMMAND} -E env DM_GAMES_DIR=${DM_GAMES_DIR})
endif()
add_custom_target(game_profile ALL
  COMMAND ${GAME_ENV} ${Python3_EXECUTABLE} ${DM}/games/game.py header ${GAME} ${GAME_H}
  BYPRODUCTS ${GAME_H} COMMENT "game profile ${GAME}")
execute_process(COMMAND ${GAME_ENV} ${Python3_EXECUTABLE} ${DM}/games/game.py header ${GAME} ${GAME_H} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "game.py header ${GAME} failed")
endif()

# The commit and version the host is built from (the Android stamp: dm/android/build_stamp.c, main.c's build: line)
set(STAMP_H ${GEN}/build_stamp.h)
add_custom_target(build_stamp ALL
  COMMAND ${Python3_EXECUTABLE} ${AND}/tools/build_stamp.py ${STAMP_H}
  BYPRODUCTS ${STAMP_H} COMMENT "build stamp")

# Assembly: the Mach-O sources converted for the COFF assembler (tools/asm_coff.py)
set(ASM_OUT)
foreach(s ${DM}/host/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  add_custom_command(OUTPUT ${GEN}/asm/${n}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_coff.py ${s} ${GEN}/asm/${n}
    DEPENDS ${s} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_coff.py)
  list(APPEND ASM_OUT ${GEN}/asm/${n})
endforeach()

# The Vulkan replay's own shaders (dm/android/shaders), compiled to SPIR-V and written as the C initializers gfx_vk.c includes (what the NDK's glslc -mfmt=c writes).
# glslc when there is one (the Vulkan SDK's), else glslang / glslangValidator and tools/spv_inc.py
find_program(GLSLC glslc)
if(NOT GLSLC)
  find_program(GLSLANG NAMES glslang glslangValidator)
  if(NOT GLSLANG)
    message(FATAL_ERROR "no glslc or glslang: install the Vulkan SDK or a glslang release (https://github.com/KhronosGroup/glslang/releases), or set -DGLSLANG=<path>")
  endif()
endif()
set(SHADER_INC)
foreach(s ds_copy.vert ds_copy_export.frag ds_copy_bits.frag bc_decode.comp gmem_test.comp gmem_test_noptr.comp gmem_probe.comp upscale_square.comp)
  if(GLSLC)
    add_custom_command(OUTPUT ${GEN}/shaders/${s}.inc
      COMMAND ${GLSLC} --target-env=vulkan1.1 -O -mfmt=c ${AND}/shaders/${s} -o ${GEN}/shaders/${s}.inc
      DEPENDS ${AND}/shaders/${s})
  else()
    add_custom_command(OUTPUT ${GEN}/shaders/${s}.inc
      COMMAND ${GLSLANG} -V --target-env vulkan1.1 ${AND}/shaders/${s} -o ${GEN}/shaders/${s}.spv
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/spv_inc.py ${GEN}/shaders/${s}.spv ${GEN}/shaders/${s}.inc
      DEPENDS ${AND}/shaders/${s} ${CMAKE_CURRENT_SOURCE_DIR}/tools/spv_inc.py)
  endif()
  list(APPEND SHADER_INC ${GEN}/shaders/${s}.inc)
endforeach()

# Host sources: every dm/host/*.c, as the Mac Makefile's wildcard, less the ones a stand-in replaces (as on Android)
file(GLOB HOST_C ${DM}/host/*.c)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_movie_avc.c ${DM}/host/hle_movie_av1.c)   # VideoToolbox / dav1d: media_windows.c (no H.264 / AV1 decoder yet: those movies are skipped)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_cheats.c)                                 # blocks and dispatch queues: stubbed in dm/android/apple_stubs.c
file(GLOB GFX_C ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
file(GLOB_RECURSE VPX_C ${DM}/third_party/libvpx/*.c)

set(DM_GFX vk CACHE STRING "NVN replay backend: vk or null")
set(WIN_C
  ${CMAKE_CURRENT_SOURCE_DIR}/plat_slots.c
  ${CMAKE_CURRENT_SOURCE_DIR}/plat_windows.c
  ${CMAKE_CURRENT_SOURCE_DIR}/posix_windows.c
  ${CMAKE_CURRENT_SOURCE_DIR}/aligned_windows.c
  ${CMAKE_CURRENT_SOURCE_DIR}/coreaudio_wasapi.c
  ${CMAKE_CURRENT_SOURCE_DIR}/media_windows.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk_driver_windows.c
  ${AND}/pac_android.c
  ${AND}/ue_dynres_android.c
  ${AND}/apple_stubs.c
  ${AND}/vk_start.c
  ${AND}/module_pack.c
  ${AND}/build_stamp.c
  ${AND}/gfx_${DM_GFX}.c)

# char is signed, as on Apple arm64 (and as Windows arm64 already makes it; stated as on Android)
add_compile_options(-fsigned-char)
add_library(vpx STATIC ${VPX_C})
target_include_directories(vpx PRIVATE ${DM}/third_party/libvpx ${DM}/third_party/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w)

set_source_files_properties(${AND}/gfx_vk.c PROPERTIES COMPILE_OPTIONS "-O2" OBJECT_DEPENDS "${SHADER_INC}")
set_source_files_properties(${DM}/gfx/nvn/hle_nvn.c PROPERTIES COMPILE_OPTIONS "-fno-omit-frame-pointer")
add_library(dmcore OBJECT ${HOST_C} ${GFX_C} ${WIN_C} ${ASM_OUT} ${SHADER_INC})
add_dependencies(dmcore game_profile build_stamp)
target_include_directories(dmcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/compat ${CMAKE_CURRENT_SOURCE_DIR} ${AND} ${DM}/host ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include
  ${DM}/third_party/vulkan-headers/include/vulkan ${GEN}/${GAME} ${GEN} ${DM}/third_party/libvpx ${DM}/third_party/stb_vorbis)
# x18 is the thread's TEB on Windows arm64: the compiler never allocates it there (stated anyway). The guest's own x18 uses are rewritten to slot K18 (patch_nso.py DM_TARGET=windows)
set(DM_CFLAGS -include dm_windows.h -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-format -Wno-deprecated-declarations -Wno-gnu-folding-constant -Wno-multichar -Wno-four-char-constants -Wno-unknown-pragmas -Wno-missing-braces
  -Wno-char-subscripts -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Werror=implicit-function-declaration)
target_compile_options(dmcore PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS}> -ffixed-x18 -march=armv8.2-a+lse)
# DM_ANDROID: the shared code's switch for the Vulkan replay's ways (hle_nvn.c's window fences, hle_svc.c's heap region), which Windows takes as Android does; the
# operating system's own differences are under _WIN32 / __ANDROID__. _FILE_OFFSET_BITS=64: a 64-bit off_t (RomFS images pass 4 GB)
set(DM_DEFS DM_WINDOWS=1 DM_ANDROID=1 _FILE_OFFSET_BITS=64 WIN32_LEAN_AND_MEAN=1 NOMINMAX=1 _WIN32_WINNT=0x0A00)
target_compile_definitions(dmcore PRIVATE ${DM_DEFS})

# vulkan-1.dll is opened at run time (vk_driver_windows.c): the build needs no Vulkan SDK
set(DM_LIBS vpx ole32 uuid winmm avrt xinput1_4 user32 gdi32 shell32 dbghelp synchronization bcrypt psapi pthread)


add_executable(dmhost_app WIN32 $<TARGET_OBJECTS:dmcore> ${CMAKE_CURRENT_SOURCE_DIR}/front_windows.c ${AND}/nvn_demo.c ${DM_RC})
target_include_directories(dmhost_app PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(dmhost_app PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -ffixed-x18>)
target_compile_definitions(dmhost_app PRIVATE ${DM_DEFS})
target_link_libraries(dmhost_app ${DM_LIBS})
target_link_options(dmhost_app PRIVATE -municode -static -Wl,--stack,8388608)   # -static: winpthreads and the C++ runtime inside, only system DLLs needed
set_target_properties(dmhost_app PROPERTIES OUTPUT_NAME dmhost)

add_executable(dmhost_cli $<TARGET_OBJECTS:dmcore> ${AND}/front_cli.c ${AND}/nvn_demo.c ${DM_RC})
target_include_directories(dmhost_cli PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(dmhost_cli PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -ffixed-x18>)
target_compile_definitions(dmhost_cli PRIVATE ${DM_DEFS})
target_link_libraries(dmhost_cli ${DM_LIBS})
target_link_options(dmhost_cli PRIVATE -static -Wl,--stack,8388608)
