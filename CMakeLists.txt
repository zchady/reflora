cmake_minimum_required(VERSION 3.25)
project(SwitchGamesRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The game this build is for: games/<NX_GAME>/ holds what only that game needs (game.cpp: nx/game.h), the shared
# runtime is the rest. One build directory per game.
set(NX_GAME "hades" CACHE STRING "Game to build (a folder under games/)")
set(NX_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${NX_GAME}")
if(NOT EXISTS "${NX_GAME_DIR}/game.cpp")
    message(FATAL_ERROR "games/${NX_GAME}/game.cpp not found")
endif()

# Generated code of the game (python -m recomp <nso> <gen dir>/<module> --name <module> --base ...). Never committed.
# Hades keeps the layout from before there were other games (private/gen); the others use private/<game>/gen.
if(NX_GAME STREQUAL "hades")
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/gen")
else()
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/${NX_GAME}/gen")
endif()
set(NX_GEN_DIR "${NX_GEN_DEFAULT}" CACHE PATH "Recompiled module sources")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-constant-logical-operand -Wno-unused-function)
endif()
if(MSVC)
    add_compile_options(/arch:AVX2)
endif()
if(MINGW)
    # llvm-mingw (the preparer's portable toolchain): the code generation clang-cl gives the MSVC build, flag for flag
    # (compared with -### on LLVM 23.1.4, WONDER_SWITCH2_PROGRESS.md §5za4): /arch:AVX2 = target-cpu haswell, tune
    # generic; clang-cl always passes -fwrapv (signed overflow wraps), -relaxed-aliasing (no type-based alias
    # analysis) and -fms-volatile, which the recompiled code and the runtime were built and tested with. CMake's
    # Release flags (-O3 -DNDEBUG) are what clang-cl's /O2 becomes. Not matched: /GS (stack protector), a security
    # check that does not change results.
    add_compile_options(-march=haswell -mtune=generic -fwrapv -fno-strict-aliasing -fms-volatile)
    # Windows 10 APIs (SetProcessDpiAwarenessContext, ...): the Windows SDK's default, mingw-w64 needs it said.
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# zstd decoder (BSD-3, runtime/third_party/zstd: the decompression sources of the 1.5.7 release): NUP packages store
# their files as zstd frames (runtime/src/nup_archive.cpp).
file(GLOB NX_ZSTD_SOURCES runtime/third_party/zstd/common/*.c runtime/third_party/zstd/decompress/*.c)
add_library(nx_zstd STATIC ${NX_ZSTD_SOURCES})
target_compile_definitions(nx_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD=0 XXH_NAMESPACE=NX_ZSTD_)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(nx_zstd PRIVATE -Wno-everything)
endif()

# Unit tests of runtime pieces that build without the game (docs/nvn/WINDOWS-PLAN.md, "Test plan"): ctest.
option(NX_TESTS "Build the standalone unit tests" OFF)
if(NX_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(gpu_pick_test tests/gpu_pick_test.cpp runtime/src/nvn/nvn_gpu_pick.cpp)
    target_compile_definitions(gpu_pick_test PRIVATE NX_GPU_PICK_STANDALONE)
    target_include_directories(gpu_pick_test PRIVATE runtime/src/nvn)
    add_test(NAME gpu_pick COMMAND gpu_pick_test)
    add_executable(tiling_test tests/tiling_test.cpp runtime/src/nvn/nvn_tiling.cpp)
    target_include_directories(tiling_test PRIVATE runtime/src/nvn)
    add_test(NAME tiling COMMAND tiling_test --selftest)
    add_executable(astc_test tests/astc_test.cpp runtime/src/nvn/nvn_astc.cpp)
    target_include_directories(astc_test PRIVATE runtime/src/nvn)
    add_test(NAME astc COMMAND astc_test --selftest)
    add_executable(nvn2_compute_test tests/nvn2_compute_test.cpp runtime/src/nvn/nvn_shaders.cpp
                   runtime/src/nvn/nvn_spirv_reflect.cpp)
    target_compile_definitions(nvn2_compute_test PRIVATE NX_SHADERS_STANDALONE)
    target_include_directories(nvn2_compute_test PRIVATE runtime/src/nvn)
    add_test(NAME nvn2_compute COMMAND nvn2_compute_test)
    add_executable(movie_test tests/movie_test.cpp runtime/src/movie_vpx.cpp)
    target_include_directories(movie_test PRIVATE runtime/src)
    add_test(NAME movie COMMAND movie_test --selftest)
    add_executable(shader_cache_test tests/shader_cache_test.cpp runtime/src/nvn/nvn_shaders.cpp
                   runtime/src/nvn/nvn_spirv_reflect.cpp)
    target_compile_definitions(shader_cache_test PRIVATE NX_SHADERS_STANDALONE)
    target_include_directories(shader_cache_test PRIVATE runtime/src/nvn)
    if(EXISTS "${CMAKE_SOURCE_DIR}/private/wonder/spirv")  # the game's own shaders: never in the repository
        add_test(NAME shader_cache COMMAND shader_cache_test "${CMAKE_SOURCE_DIR}/private/wonder")
    endif()
    add_executable(nup_archive_test tests/nup_archive_test.cpp runtime/src/nup_archive.cpp)
    target_include_directories(nup_archive_test PRIVATE runtime/src)
    target_link_libraries(nup_archive_test PRIVATE nx_zstd)
    add_test(NAME nup_archive COMMAND nup_archive_test "${CMAKE_SOURCE_DIR}/tests/data/synthetic.nup")
    if(WIN32)
        add_executable(memguard_test tests/memguard_test.cpp)
        target_link_libraries(memguard_test PRIVATE psapi)
        add_test(NAME memguard COMMAND memguard_test)
        add_executable(sleep_test tests/sleep_test.cpp runtime/src/sleep_win.cpp)
        target_include_directories(sleep_test PRIVATE runtime/src)
        target_compile_definitions(sleep_test PRIVATE NX_SLEEP_STANDALONE)
        add_test(NAME sleep COMMAND sleep_test)
    endif()
endif()

# Without the generated code (a checkout with no game data) only the tests above are built.
file(GLOB NX_GEN_PROBE "${NX_GEN_DIR}/main/*.cpp")
if(NOT NX_GEN_PROBE)
    message(WARNING "No generated code in ${NX_GEN_DIR}/main: building the unit tests only")
    return()
endif()

# The recompiled modules: main and sdk, plus any subsdk<N> generated next to them (this game has
# subsdk0, nn::movie). nx_modules.cpp lists them for the loader (runtime/src/main.cpp) in load order.
set(NX_MODULES main sdk)
foreach(k RANGE 0 9)
    if(EXISTS "${NX_GEN_DIR}/subsdk${k}/table.cpp")
        list(APPEND NX_MODULES subsdk${k})
    endif()
endforeach()
set(NX_MODULES_DECL "")
set(NX_MODULES_LIST "")
foreach(mod ${NX_MODULES})
    string(APPEND NX_MODULES_DECL "extern const nx::ModuleInfo nx_module_${mod};\n")
    string(APPEND NX_MODULES_LIST "&nx_module_${mod}, ")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/nx_modules.cpp" CONTENT
"// Generated by CMakeLists.txt: the recompiled modules of this build, in load order.\n#include <cstddef>\n#include <nx/module.h>\n\n${NX_MODULES_DECL}\nnamespace nx {\nextern const ModuleInfo* const g_module_list[] = {${NX_MODULES_LIST}};\nextern const size_t g_module_count = sizeof g_module_list / sizeof g_module_list[0];\n}  // namespace nx\n")
message(STATUS "Recompiled modules: ${NX_MODULES}")
list(TRANSFORM NX_MODULES PREPEND game_ OUTPUT_VARIABLE NX_MODULE_LIBS)

foreach(mod ${NX_MODULES})
    file(GLOB src CONFIGURE_DEPENDS "${NX_GEN_DIR}/${mod}/*.cpp")
    add_library(game_${mod} STATIC ${src})
    target_include_directories(game_${mod} PRIVATE runtime/include "${NX_GEN_DIR}/${mod}")
    if(ANDROID)
        # Hidden visibility: calls between recompiled functions are direct, not through the PLT.
        # armv8.2-a+crypto: cpu.h's AES/SHA paths need __ARM_FEATURE_AES/SHA2 (the NDK default armv8-a has neither).
        target_compile_options(game_${mod} PRIVATE -O2 -fPIC -fvisibility=hidden -fno-semantic-interposition
                               -march=armv8.2-a+crypto)
    elseif(APPLE)
        target_compile_options(game_${mod} PRIVATE -O2 -fvisibility=hidden)
    endif()
endforeach()

set(NX_RUNTIME_SOURCES
    ${CMAKE_BINARY_DIR}/nx_modules.cpp
    runtime/src/main.cpp
    runtime/src/nup_archive.cpp
    runtime/src/content.cpp
    runtime/src/loader.cpp
    runtime/src/memory.cpp
    runtime/src/kernel.cpp
    runtime/src/kernel_watch.cpp
    runtime/src/timeline.cpp
    runtime/src/collect.cpp
    runtime/src/hitch.cpp
    runtime/src/ipc.cpp
    runtime/src/svc_am.cpp
    runtime/src/svc_misc.cpp
    runtime/src/svc_time.cpp
    runtime/src/svc_process.cpp
    runtime/src/hle_fs.cpp
    runtime/src/svc_account.cpp
    runtime/src/hle_ro.cpp
    runtime/src/hle_vi.cpp
    runtime/src/hle_hid.cpp
    runtime/src/hle_net.cpp
    runtime/src/hle_audio.cpp
    runtime/src/hle_pl.cpp
    runtime/src/hle_nfp.cpp
    runtime/src/hle_streamplay.cpp
    runtime/src/hle_movie.cpp
    runtime/src/movie_vpx.cpp
    runtime/src/input_script.cpp
    runtime/src/touch_layout.cpp
    runtime/src/perf_hint.cpp
    runtime/src/frame_stats.cpp
    runtime/src/write_watch.cpp
    runtime/src/nvn/nvn_caps.cpp
    runtime/src/nvn/nvn_renderpass.cpp
    runtime/src/nvn/nvn_gpu_busy.cpp
    runtime/src/nvn/nvn_janitor.cpp
    runtime/src/nvn/nvn_trace.cpp
    runtime/src/nvn/nvn_device.cpp
    runtime/src/nvn/nvn_objects.cpp
    runtime/src/nvn/nvn_incarnation.cpp
    runtime/src/nvn/nvn_texture_layout.cpp
    runtime/src/nvn/nvn_program.cpp
    runtime/src/nvn/nvn_queue.cpp
    runtime/src/nvn/nvn_present_log.cpp
    runtime/src/nvn/nvn_cmd.cpp
    runtime/src/nvn/nvn_state.cpp
    runtime/src/nvn/nvn_format.cpp
    runtime/src/nvn/nvn_enums.cpp
    runtime/src/nvn/nvn_sampler.cpp
    runtime/src/nvn/nvn_sampler_objects.cpp
    runtime/src/nvn/nvn_draw_state.cpp
    runtime/src/nvn/nvn_raster.cpp
    runtime/src/nvn/nvn_worker.cpp
    runtime/src/nvn/nvn_shaders.cpp
    runtime/src/nvn/nvn_spirv_reflect.cpp
    runtime/src/nvn/nvn_tiling.cpp
    runtime/src/nvn/nvn_vk.cpp
    runtime/src/nvn/nvn_gpu_pick.cpp
    runtime/src/nvn/nvn_image.cpp
    runtime/src/nvn/nvn_image_memory.cpp
    runtime/src/nvn/nvn_astc_cache.cpp
    runtime/src/nvn/nvn_frames.cpp
    runtime/src/nvn/nvn_dedup.cpp
    runtime/src/nvn/nvn_written.cpp
    runtime/src/nvn/nvn_replay.cpp
    runtime/src/nvn/nvn_debug.cpp
    runtime/src/nvn/nvn_binds.cpp
    runtime/src/nvn/nvn_copy.cpp
    runtime/src/nvn/nvn_pipeline.cpp
    runtime/src/nvn/nvn_dummy.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_pipeline_catalog.cpp
    runtime/src/nvn/nvn_pipeline_state.cpp
    runtime/src/nvn/nvn_upload.cpp
    runtime/src/nvn/nvn_astc.cpp
    runtime/src/nvn/nvn_spirv_patch.cpp
    runtime/src/nvn/nvn_spirv_clip.cpp
    runtime/src/nvn/nvn_present.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_bc.cpp
    runtime/src/nvn/nvn_draw.cpp
    runtime/src/nvn/nvn_sets.cpp
    runtime/src/nvn/nvn_clears.cpp
    runtime/src/nvn/nvn_mask_clear.cpp
    runtime/src/nvn/nvn_draw_trace.cpp
    runtime/src/nvn/nvn_event.cpp
    runtime/src/vk_bridge.cpp
    runtime/src/vk_swapchain.cpp
    runtime/src/vk_gen.cpp
    runtime/src/vk_present.cpp
)
file(GLOB NX_GAME_SOURCES CONFIGURE_DEPENDS "${NX_GAME_DIR}/*.cpp")
list(APPEND NX_RUNTIME_SOURCES ${NX_GAME_SOURCES})

# Vulkan headers (the same registry version tools/gen_vk.py generated from). The loader comes with the GPU driver.
if(DEFINED ENV{VULKAN_SDK})
    set(NX_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK")
else()
    set(NX_VULKAN_SDK "C:/VulkanSDK/1.4.357.0" CACHE PATH "Vulkan SDK")
endif()
# Only vulkan/vulkan.h is used: a Vulkan-Headers checkout (the preparer's pinned vulkan-sdk-1.4.363.0) is enough.
set(NX_VULKAN_INCLUDE "${NX_VULKAN_SDK}/Include" CACHE PATH "Directory holding vulkan/vulkan.h")

if(NX_TESTS AND NOT ANDROID)
    # The canonical pipeline key against the Vulkan specification's reading of the create info (no device).
    add_executable(pipeline_key_test tests/pipeline_key_test.cpp runtime/src/nvn/nvn_pipeline_state.cpp
                   runtime/src/nvn/nvn_enums.cpp runtime/src/nvn/nvn_format.cpp)
    target_include_directories(pipeline_key_test PRIVATE runtime/include runtime/src runtime/src/nvn "${NX_VULKAN_INCLUDE}")
    add_test(NAME pipeline_key COMMAND pipeline_key_test)
endif()
# An SDL3 package (a prefix with lib/cmake/SDL3), e.g. the official SDL3-devel-*-mingw for the preparer's build.
# Empty: MSYS2's SDL3 when installed (development machines), else no SDL3 (XInput only).
set(NX_SDL3_DIR "" CACHE PATH "SDL3 package prefix (empty: MSYS2's, if any)")

if(ANDROID)
    # NativeActivity app: lib<game>.so, entry android_main (android_native_app_glue).
    set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(${NX_GAME} SHARED ${NX_RUNTIME_SOURCES} runtime/src/platform_android.cpp
        runtime/src/platform_android_input.cpp runtime/src/platform_android_audio.cpp runtime/src/memguard_android.cpp
        "${GLUE}/android_native_app_glue.c")
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_SDK}/Include" "${GLUE}")
    # Our ANativeActivity_onCreate (platform_android.cpp) runs first, on the UI thread, then calls the glue's.
    set_source_files_properties("${GLUE}/android_native_app_glue.c" PROPERTIES
        COMPILE_DEFINITIONS "ANativeActivity_onCreate=glue_ANativeActivity_onCreate")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_MODULE_LIBS} nx_zstd android log aaudio)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -fno-semantic-interposition -march=armv8.2-a+crypto)
    # The glue's ANativeActivity_onCreate must be exported. 16 KB page alignment: Android 15+ devices with 16 KB pages.
    target_link_options(${NX_GAME} PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
elseif(APPLE)
    # macOS: SDL3 (window, input, audio) and the Vulkan loader + MoltenVK from Homebrew
    # (brew install sdl3 vulkan-headers vulkan-loader molten-vk).
    find_package(SDL3 REQUIRED CONFIG)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS /opt/homebrew/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_mac.cpp runtime/src/memguard_mac.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_MODULE_LIBS} nx_zstd SDL3::SDL3)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden)
    # The executable loads near 0x1_0000_0000, below the guest ranges (0x40_0000_0000 and up). The game runs on its
    # own thread with a 64 MiB stack (platform_mac.cpp), so the main thread's stack size does not matter.
else()
    # Dear ImGui (MIT, runtime/third_party/imgui: the 1.91.9 core and its Vulkan back end) for the settings overlay
    # (runtime/src/overlay.cpp); its Vulkan calls go through the runtime's loader (IMGUI_IMPL_VULKAN_NO_PROTOTYPES).
    add_library(nx_imgui STATIC runtime/third_party/imgui/imgui.cpp runtime/third_party/imgui/imgui_draw.cpp
                runtime/third_party/imgui/imgui_tables.cpp runtime/third_party/imgui/imgui_widgets.cpp
                runtime/third_party/imgui/backends/imgui_impl_vulkan.cpp)
    target_include_directories(nx_imgui PUBLIC runtime/third_party/imgui PRIVATE "${NX_VULKAN_INCLUDE}")
    target_compile_definitions(nx_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(nx_imgui PRIVATE -Wno-everything)
    endif()
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_win.cpp runtime/src/sleep_win.cpp
                   runtime/src/memguard_win.cpp runtime/src/vblank_win.cpp runtime/src/settings.cpp
                   runtime/src/overlay.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    if(MINGW)
        # xinput1_4 / xaudio2_9: the DLLs the MSVC import libraries name (both ship with Windows 10+; mingw's plain
        # "xinput" is xinput1_3, from the DirectX redistributable).
        target_link_libraries(${NX_GAME} PRIVATE ${NX_MODULE_LIBS} nx_zstd nx_imgui xinput1_4 xaudio2_9 ole32 psapi dwmapi)
    else()
        target_link_libraries(${NX_GAME} PRIVATE ${NX_MODULE_LIBS} nx_zstd nx_imgui xinput xaudio2 ole32 psapi dwmapi)
    endif()
    if(NX_GAME STREQUAL "wonder2")
        set_property(SOURCE runtime/src/memguard_win.cpp APPEND PROPERTY COMPILE_DEFINITIONS NX_DEFAULT_MEM_LIMIT_GB=20)
    endif()
    if(NX_SDL3_DIR)
        find_package(SDL3 CONFIG REQUIRED PATHS "${NX_SDL3_DIR}" NO_DEFAULT_PATH)
    else()
        find_package(SDL3 CONFIG QUIET HINTS "C:/msys64/ucrt64")
    endif()
    if(TARGET SDL3::SDL3-shared)
        # Stage only SDL headers: an MSYS2 prefix also contains C runtime headers incompatible with clang-cl.
        find_path(NX_SDL3_INCLUDE SDL3/SDL.h HINTS "${SDL3_DIR}/../../.." PATH_SUFFIXES include REQUIRED)
        file(COPY "${NX_SDL3_INCLUDE}/SDL3" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/sdl-headers")
        set_property(SOURCE runtime/src/platform_win.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/sdl-headers")
        set_property(SOURCE runtime/src/platform_win.cpp APPEND PROPERTY COMPILE_DEFINITIONS NX_SDL_GAMEPAD)
        target_link_libraries(${NX_GAME} PRIVATE "$<TARGET_LINKER_FILE:SDL3::SDL3-shared>")
        add_custom_command(TARGET ${NX_GAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3-shared>" "$<TARGET_FILE_DIR:${NX_GAME}>")
        # MSYS2's SDL build also imports libiconv. Native SDL distributions do not require it.
        if(EXISTS "${NX_SDL3_INCLUDE}/../bin/libiconv-2.dll")
            add_custom_command(TARGET ${NX_GAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NX_SDL3_INCLUDE}/../bin/libiconv-2.dll" "$<TARGET_FILE_DIR:${NX_GAME}>")
        endif()
    endif()
    # The runtime uses the portable C library (getenv, fopen, ...) on every host: no "_s" variants.
    target_compile_definitions(${NX_GAME} PRIVATE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    if(MSVC)
        # Keep host allocations low so the guest ranges (0x40_0000_0000..0x70_0000_0000) are always free; a big
        # main-thread stack (recompiled calls nest deeply); a link map for tools/symbolize.py.
        target_link_options(${NX_GAME} PRIVATE /STACK:67108864 /HIGHENTROPYVA:NO /MAP)
    endif()
    if(MINGW)
        # The same three as above for lld in MinGW mode; -static: libc++, libunwind and winpthreads inside the
        # executable, so it needs only Windows' own DLLs (UCRT included) and SDL3.dll.
        target_link_options(${NX_GAME} PRIVATE -static -Wl,--stack,67108864 -Wl,--disable-high-entropy-va
                            "-Wl,-Map,$<TARGET_FILE_DIR:${NX_GAME}>/${NX_GAME}.map")
    endif()
endif()
