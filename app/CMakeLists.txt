# Sonic Adventure 2 (Xbox Live Arcade): static recompilation host application.
#
# The game code is not in this repository: run the code generator first (tools/codegen.ps1), which writes generated/
# from your own default.xex together with the CMake glue (generated/rexglue.cmake, made by `rexglue codegen`).
#
# Stage 1, Windows PC:
#   cmake --preset win-pc && cmake --build --preset win-pc        (from app/, inside tools\win\env.ps1)
# Stage 2, Nintendo Switch (devkitA64), later:
#   cmake -S app -B out/nx -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake -DREXGLUE_SWITCH_NVK_SDK=<mesa sdk>

cmake_minimum_required(VERSION 3.25)
project(sa2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR "app/generated/rexglue.cmake is missing: run tools/codegen.ps1 first")
endif()
include(generated/rexglue.cmake)

# The build re-runs the generator by itself whenever an input in codegen.d changes (manifest, toml, xex, the generator
# binary). That rewrites app/generated and would silently drop the codegen flavour's post-codegen steps (no-volatile
# pch, direct calls, localized natives): they are appended to the same command (docs/enhancements.md, "Build-time").
# The Switch cross build has no generator target, so this only applies on the PC.
if(TARGET sa2_codegen)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp" APPEND
        COMMAND "${CMAKE_COMMAND}" -E env "REXSDK_DIR=${REXSDK_DIR}"
                "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/post_codegen.py")
endif()

set(SA2_SOURCES
    src/main.cpp
    # Stage 2, phase A: wraps the game's Direct3D candidates and logs them (cvar sa2_d3d_trace, off by default).
    src/sa2_d3d_trace.cpp
    # Video recordings of the game image for testing (cvar sa2_video_s; tools/make_video.sh makes the MP4).
    src/sa2_capture.cpp
    # DLC: installs the content packages in dlc/ next to the executable (cvar sa2_dlc_dir).
    src/sa2_dlc.cpp
    # The game's language (cvar language: auto = the system language).
    src/sa2_language.cpp
    # PGO profile collection (the Switch build with SA2_PGO=generate; empty otherwise).
    src/sa2_pgo.cpp
    # Windows: logs fatal host exceptions the SDK does not report (illegal instruction, ...) with module + offset.
    src/sa2_crash_log.cpp
    # Temporary hooks that log the callers of functions a boot crashed in.
    src/sa2_debug_hooks.cpp
    # Stage 2: the native renderer (src/me, masseffect-nx's PM4-ring Vulkan renderer as adapted by banjo-nx). It
    # replaces the emulated Xenos GPU with sa2_renderer_native = true; off by default until it draws SA2 right.
    src/me/me_native_system.cpp
    src/me/me_ring_wait.cpp
    src/me/me_renderdoc.cpp
    src/me/native/sa2_native_draws.cpp
    src/me/native/sa2_deferred_recording.cpp
    src/me/native/sa2_native_targets.cpp
    src/me/native/sa2_native_shaders.cpp
    src/me/native/sa2_native_textures_pool.cpp
    src/me/native/sa2_shader_library.cpp
    src/me/native/sa2_native_glue.cpp
    # SA2's D3D addresses (draw-time shader load, Draw*) and the shader container dump (sa2_dump_shaders).
    src/me/sa2_me_hooks.cpp
    src/me/sa2_d3d_api.cpp
    # The native Direct3D layer (docs/api-renderer.md): Direct3D entry points as native code writing the ring's records.
    src/me/sa2_api_draw.cpp
    src/me/sa2_api_clear.cpp
    src/me/sa2_api_resolve.cpp
    # Render resolution per console mode (docked / handheld, live) and MSAA: sa2_render_height_*, sa2_msaa.
    src/me/sa2_render_mode.cpp
    # Switchable enhancements (docs/enhancements.md): masters sa2_cpu_enhancements / sa2_noncpu_enhancements, one key
    # each, a startup line and a 10 s use report.
    src/cpu/sa2_enhancements.cpp
    src/cpu/sa2_gpu_wait.cpp
    src/cpu/sa2_gpu_boost.cpp
    # Native replacements of hot guest functions (src/cpu/hot), each with a self-check guard and its own key.
    src/cpu/sa2_hot_guest.cpp
)
# Diagnostic builds only (e.g. the Direct3D census: out/census/sa2_d3d_census_all.cpp): extra sources, empty by default.
set(SA2_EXTRA_SOURCES "" CACHE STRING "Extra source files for diagnostic builds (empty for normal builds)")
if(SA2_EXTRA_SOURCES)
    list(APPEND SA2_SOURCES ${SA2_EXTRA_SOURCES})
endif()
if(NOT REXGLUE_PLATFORM_SWITCH)
    # Switch profiler counters the native renderer calls (no-ops on PC).
    list(APPEND SA2_SOURCES src/me/me_pc_stubs.cpp)
    # Sampling CPU profiler for PC tests (from nfsmw-nx via banjo-nx; sa2_cpu_profile_from_s, logs/perfil_pc.csv).
    list(APPEND SA2_SOURCES src/profiler/sa2_cpu_profile_pc.cpp)
endif()

# The codegen flavour (tools/codegen.ps1 -Flavor) names the build in the first "[enh]" log line.
set(SA2_FLAVOR_FILE "${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen_flavor.txt")
set(SA2_BUILD_FLAVOR "classic codegen")
if(EXISTS "${SA2_FLAVOR_FILE}")
    file(READ "${SA2_FLAVOR_FILE}" SA2_BUILD_FLAVOR)
    string(STRIP "${SA2_BUILD_FLAVOR}" SA2_BUILD_FLAVOR)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SA2_FLAVOR_FILE}")
endif()
set_source_files_properties(src/cpu/sa2_enhancements.cpp PROPERTIES
    COMPILE_DEFINITIONS "SA2_BUILD_FLAVOR=\"${SA2_BUILD_FLAVOR}\"")

if(WIN32)
    add_executable(sa2 WIN32 ${SA2_SOURCES})
else()
    add_executable(sa2 ${SA2_SOURCES})
endif()

# Link the SDK. Stage 1 draws with ReXGlue's Xenos GPU emulation; the native renderer replaces it later.
rexglue_setup_target(sa2 GPU_PLUGINS xenos)

# SPIR-V initializers of the native renderer's EDRAM transfer / utility shaders (src/me/native/shaders), compiled
# on the host with glslangValidator (tools/win/build_glslang.ps1 builds it from the SDK's glslang). The Switch build
# uses the same host tool: SPIR-V is platform independent.
set(SA2_GLSLANG_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/../out/glslang/StandAlone/glslangValidator${CMAKE_EXECUTABLE_SUFFIX}"
    CACHE FILEPATH "Host glslangValidator")
set(SA2_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
set(SA2_EDRAM_SHADER_INCLUDES)
foreach(_shader sa2_edram_7e3_to_rgba8 sa2_edram_rgba8_to_7e3 sa2_edram_16f_to_16f me_resolve_exp_bias me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_import me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2 me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_depth_to_raw64 me_edram_raw64_to_depth me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2 me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_edram_color_to_color me_edram_r64_to_r64 sa2_resolve_copy sa2_depth_resolve_ms)
    set(_extension comp)
    if(_shader STREQUAL "me_edram_import")
        set(_extension vert)
    elseif(_shader MATCHES "^me_edram_(color_to_depth|depth_to_depth|depth_to_stencil|color_to_stencil|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x|raw64_to_depth|color_to_color|r64_to_r64)$" OR _shader STREQUAL "sa2_resolve_copy" OR
           _shader STREQUAL "sa2_depth_resolve_ms")
        set(_extension frag)
    endif()
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/src/me/native/shaders/${_shader}.${_extension}")
    set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}.inc")
    set(_target_flags)
    if(_shader MATCHES "^me_edram_(stencil_to_buffer|color_stencil_to_buffer|stencil_msaa2_to_buffer|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x)$" OR _shader STREQUAL "me_depth_resolve_guestspace_msaa2")
        set(_target_flags --target-env vulkan1.2)
        set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}_generated.inc")
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SA2_GENERATED_SHADER_DIR}"
        COMMAND "${SA2_GLSLANG_VALIDATOR}" -V ${_target_flags} -x -o "${_output}" "${_source}"
        DEPENDS "${_source}" VERBATIM)
    list(APPEND SA2_EDRAM_SHADER_INCLUDES "${_output}")
endforeach()
add_custom_target(sa2_edram_shaders DEPENDS ${SA2_EDRAM_SHADER_INCLUDES})
add_dependencies(sa2 sa2_edram_shaders)
target_include_directories(sa2 PRIVATE
    "${SA2_GENERATED_SHADER_DIR}"
    src
    src/me
    src/me/native
    src/me/native/shaders
    "${REXSDK_DIR}/src/core"
    # Precompiled SPIR-V of the SDK presenter (vulkan_spirv/guest_output_*): the renderer draws into its output.
    "${REXSDK_DIR}/src/ui/shaders")

# CPU-only SDK sources the native renderer reuses (draw-extent estimation, shader microcode interpretation): the
# GPU plugin's private symbols cannot satisfy application-side use.
target_sources(sa2 PRIVATE
    "${REXSDK_DIR}/src/graphics/register_file.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")

# The renderer includes xxhash.h and renderdoc_app.h directly; when the SDK is built from source they are private to
# its libraries.
foreach(_dep xxhash renderdoc)
    if(TARGET ${_dep})
        target_include_directories(sa2 PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# Settings template next to the executable, only when there is none yet (the copy there is the user's to edit).
if(NOT REXGLUE_PLATFORM_SWITCH)
    add_custom_command(TARGET sa2 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/sa2.toml -DDST=$<TARGET_FILE_DIR:sa2>/sa2.toml
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake"
        VERBATIM)
endif()

# The SDK compiles itself with these (directory scope), but they do not reach the generated game code, which is
# built in this directory. GCC's default for GNU C++ is -ffp-contract=fast: it fuses separate PPC fmul/fadd into
# FMA and results drift from the Xbox 360; strict aliasing is unsafe for recompiled code. Every target that holds
# generated code needs them: the executable, sa2_recomp and each guest module (sa2_Data_DLL, the stage code, was
# missing until 2026-10-06). Clang (the PC build) gets them too, so the PC rounds exactly like the console.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target sa2 sa2_recomp sa2_Data_DLL)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
        endif()
    endforeach()
endif()

# sa2_register.cpp is one generated function of ~30,000 SetFunction calls that runs once at startup; GCC -O3 spent
# 771-927 s on it alone (the critical path of every Switch build). -O0 compiles it in seconds and changes nothing at
# run time but a few microseconds of startup. (From sasrt-nx, 2026-10-06.)
if(REXGLUE_PLATFORM_SWITCH)
    foreach(register_file generated/default/sa2_register.cpp generated/data_dll/sa2_register.cpp)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${register_file}")
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${register_file}" APPEND PROPERTY COMPILE_OPTIONS -O0)
        endif()
    endforeach()
endif()

if(REXGLUE_PLATFORM_SWITCH AND TARGET sa2_Data_DLL)
    # The guest DLL module goes into the NRO (no dynamic loading on the console): its static archive (CMake builds
    # the generated SHARED library as STATIC on this platform) is partially linked into one object, every symbol
    # the module did not export is made local (it is compiled with hidden visibility, as on PC, and its generated
    # globals - PPCFuncMappings, xstart, ... - share names with the executable's), and the two entry points are
    # renamed for src/sa2_static_modules.cpp, which registers them.
    set(_dll_object "${CMAKE_CURRENT_BINARY_DIR}/sa2_Data_DLL_linked.o")
    get_filename_component(_switch_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(OUTPUT "${_dll_object}"
        # --force-group-allocation: COMDAT groups (inline functions) become ordinary sections resolved inside the
        # module; left as groups, the final link could keep the executable's copy of a group whose symbols
        # --localize-hidden made local here, and drop the module's.
        COMMAND "${_switch_bin}/aarch64-none-elf-ld" -r --force-group-allocation -o "${_dll_object}.tmp"
                --whole-archive $<TARGET_FILE:sa2_Data_DLL> --no-whole-archive
        COMMAND "${CMAKE_OBJCOPY}" --localize-hidden
                --redefine-sym ReXModule_Register=ReXModule_Register_sa2_Data_DLL
                --redefine-sym ReXModule_GetImageInfo=ReXModule_GetImageInfo_sa2_Data_DLL
                "${_dll_object}.tmp" "${_dll_object}"
        DEPENDS sa2_Data_DLL
        COMMENT "Linking the Data_DLL module into one object for the NRO"
        VERBATIM)
    set_source_files_properties("${_dll_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(sa2 PRIVATE "${_dll_object}" src/sa2_static_modules.cpp)
endif()

# Switch compiler optimisations (2026-10-06, the user's choice: UnleashedRecomp-NX's set without PGO, at -O3):
#   - LTO (nfsmw-nx / UnleashedRecomp-NX): the recompiled game (sa2_recomp) and the app (renderer, natives) are compiled
#     to GIMPLE (-flto -fno-fat-lto-objects) and optimised as one program at the link (-flto=N -flto-partition=balanced:
#     N parallel LTRANS jobs, kept low for the build machine's memory). Calls between unhooked game functions are
#     already direct (tools/direct_calls.py), so small game functions inline across files. The SDK libraries and the
#     stage module (sa2_Data_DLL, partially linked with ld -r + objcopy, which needs machine code) stay out.
#   - -fipa-pta (UnleashedRecomp-NX): interprocedural points-to analysis at the LTO link.
#   - The link repeats -ffp-contract=off and -fno-strict-aliasing (the LTRANS stage must not fuse what the compile
#     stage kept apart), and xxHash is built with XXH_FORCE_MEMORY_ACCESS=0: nfsmw-nx's LTO+PGO builds hit xxHash's
#     default (a strict-aliasing violation) once it was inlined into a cache-key function (garbage keys).
#   - Hot functions first in .text (nfsmw-nx's orden_funciones.ld): app/hot_functions.ld, from console stack samples
#     (tools/hot_order.py). It starts with KEEP(*(.crt0)): the start-up code must stay at address 0
#     (tools/check_switch_build.py checks it).
if(REXGLUE_PLATFORM_SWITCH)
    option(SA2_SWITCH_LTO "Link-time optimisation of the game code and the app (Switch)" ON)
    set(SA2_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel LTRANS jobs of the Switch LTO link")
    option(SA2_SWITCH_IPA_PTA "-fipa-pta at the Switch LTO link" ON)
    option(SA2_SWITCH_HOT_ORDER "Hot functions first in .text (app/hot_functions.ld)" ON)
    target_compile_definitions(sa2 PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
    if(SA2_SWITCH_LTO)
        set(_sa2_lto_compile -flto -fno-fat-lto-objects)
        set(_sa2_lto_link -flto=${SA2_SWITCH_LTO_JOBS} -flto-partition=balanced -ffp-contract=off -fno-strict-aliasing)
        if(SA2_SWITCH_IPA_PTA)
            list(APPEND _sa2_lto_compile -fipa-pta)
            list(APPEND _sa2_lto_link -fipa-pta)
        endif()
        foreach(target sa2_recomp sa2)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_sa2_lto_compile})
            endif()
        endforeach()
        target_link_options(sa2 PRIVATE ${_sa2_lto_link})
        message(STATUS "sa2: Switch LTO on (${SA2_SWITCH_LTO_JOBS} LTRANS jobs, -fipa-pta ${SA2_SWITCH_IPA_PTA})")
    endif()
    # Line tables for the app's own code (renderer, natives, hooks): console profiles (rex_profile.log PCs) map to
    # source lines with addr2line on the archived ELF. -g1 does not change the generated code (GCC keeps code
    # generation independent of -g); the NRO is made from the stripped ELF. The generated game code stays without.
    option(SA2_SWITCH_LINE_INFO "Line tables (-g1) for the app's own code in the Switch ELF" ON)
    if(SA2_SWITCH_LINE_INFO)
        # DWARF 4: with DWARF 5 the LTO output mapped some functions' lines to the wrong file (file index mix-up).
        target_compile_options(sa2 PRIVATE -g1 -gdwarf-4)
        target_link_options(sa2 PRIVATE -g1 -gdwarf-4)
    endif()
    # Profile-guided optimisation (2026-10-08, the user's request), for the game code and the app like LTO:
    #   SA2_PGO=generate  an instrumented NRO that writes its branch counts to pgo/ on the SD card (src/sa2_pgo.cpp;
    #                     registered in a section, not by constructors: libgcov's exit-time files do not fit the
    #                     Switch). Value profiling is off: its shared lists are not safe without atomic updates.
    #   SA2_PGO=use       the counts merged on the PC (tools/win/pgo_merge.ps1). Code the sessions did not run is
    #                     compiled as without a profile (-fprofile-partial-training), and functions changed since the
    #                     instrumented build keep no profile (-Wno-coverage-mismatch).
    # The profile is pgo/ at the root of the repository (SA2_PGO_DIR): one .gcda per object, named by the object's
    # path in the build folder with # for each folder separator. -fprofile-prefix-path makes GCC use those relative
    # names, so the profile works from any checkout; it must be the build folder written the Windows way, as GCC
    # sees it. pgo/ holds the profile of the released NRO.
    set(SA2_PGO "" CACHE STRING "Profile-guided optimisation of the Switch build: empty, generate or use")
    set_property(CACHE SA2_PGO PROPERTY STRINGS "" generate use)
    set(SA2_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
    get_filename_component(_sa2_pgo_dir "${SA2_PGO_DIR}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_sa2_pgo_dir}" _sa2_pgo_dir)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _sa2_pgo_prefix)
    if(SA2_PGO STREQUAL "generate")
        set(_sa2_pgo "-fprofile-generate=${_sa2_pgo_dir}" "-fprofile-prefix-path=${_sa2_pgo_prefix}"
                     -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(sa2 PRIVATE SA2_PGO_GENERATE=1)
    elseif(SA2_PGO STREQUAL "use")
        if(NOT EXISTS "${SA2_PGO_DIR}")
            message(FATAL_ERROR "SA2_PGO=use: no profile in ${SA2_PGO_DIR}")
        endif()
        # -fprofile-correction: the counts were taken without atomic updates (-fprofile-update=single) while several
        # game threads ran the same code, so a few increments were lost and some blocks' counts do not add up ("not
        # flow-consistent"); GCC smooths them instead of stopping.
        set(_sa2_pgo "-fprofile-use=${_sa2_pgo_dir}" "-fprofile-prefix-path=${_sa2_pgo_prefix}" -fprofile-correction
                     -fno-profile-values -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch)
    elseif(SA2_PGO)
        message(FATAL_ERROR "SA2_PGO must be empty, generate or use (got ${SA2_PGO})")
    endif()
    if(SA2_PGO)
        foreach(target sa2_recomp sa2)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_sa2_pgo})
            endif()
        endforeach()
        target_link_options(sa2 PRIVATE ${_sa2_pgo})
        message(STATUS "sa2: PGO ${SA2_PGO}")
    endif()
    if(SA2_SWITCH_HOT_ORDER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hot_functions.ld")
        target_link_options(sa2 PRIVATE "-Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/hot_functions.ld")
        set_property(TARGET sa2 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hot_functions.ld")
    endif()
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # The Switch build's fixed settings, compiled in (src/sa2_app.h BuiltInConfig): everything sa2.toml no longer
    # lists. The SD card's sa2.toml holds the player's options and overrides these key by key.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sa2_switch_defaults.toml" SA2_BUILTIN_CONFIG)
    # And the player's sa2.toml, written next to the NRO when it is missing (src/sa2_app.h DefaultConfigFile).
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sa2_switch.toml" SA2_PLAYER_CONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sa2_switch.toml")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/sa2_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/sa2_builtin_config.h" @ONLY)
    target_include_directories(sa2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(sa2 PRIVATE SA2_BUILTIN_CONFIG=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sa2_switch_defaults.toml")
    # The homebrew menu's entry: name, author, version and icon (app/icon.jpg, 256x256 JPEG).
    rexglue_switch_add_nro(sa2
        NAME "Sonic Adventure 2"
        AUTHOR "ChanseyIsTheBest"
        VERSION "1.0.0"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
endif()
