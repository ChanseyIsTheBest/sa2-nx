# renut - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(renut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# banjo-nx (from peggle2-nx, 2026-10-13): -mearly-ra=none for everything compiled for the console. devkitA64 GCC 16.1
# runs an AArch64 "early register allocation" pass at -O2 and above (-mearly-ra=all). In Peggle 2's console build it
# dropped 38 of the table lookups of the XDK's streaming copy (the two-register lookup of the NEON vperm helper,
# rex/ppc/intrinsics.h: V2x16QI pseudos that need consecutive registers) and overwrote the register holding a
# constant: large textures came out as garbage on the console only (clang builds the PC code correctly, so no PC check
# can see it). Banjo's ELF of 2026-10-13 shows the same routine complete (159 lookups) and no self-indexed lookup
# anywhere, but the trigger is there. Here, before the SDK is added, so its libraries and any later LTO link get it
# too (a toolchain-file flag would only reach a fresh build folder: CMake caches those). Checked after a build: the
# flag in every compile command, and no `tbl vA, {vB}, vB` in a recompiled function. BANJO_SWITCH_EARLY_RA=ON puts
# the compiler's default back.
option(BANJO_SWITCH_EARLY_RA "Leave GCC's AArch64 early register allocation pass on (Switch; miscompiles, see above)" OFF)
# Line tables (-g1) for the three hot renderer files, so tools/switch_hot_lines.py can say where inside a function a
# console profile's samples fall. -g does not change the generated code: an ELF built with this ON has every function
# at the address of the same sources built with it OFF, so it names the samples of a run of the normal NRO.
option(BANJO_SWITCH_RENDERER_LINES "Switch: line tables for the hot native renderer files (profiling aid)" OFF)
if(REXGLUE_PLATFORM_SWITCH AND NOT BANJO_SWITCH_EARLY_RA)
    add_compile_options(-mearly-ra=none)
    add_link_options(-mearly-ra=none)
endif()
# banjo-nx (package 27, codegen round): atomics inline. GCC's default for AArch64 calls out-of-line helpers
# (__aarch64_cas4_acq, __aarch64_ldadd8_rel, ...) that test for LSE atomics at run time; the A57 has none, so every
# atomic was a call, a load and a branch before the same load-exclusive / store-exclusive loop this writes inline (75
# call sites in the ELF of package 26: shared_ptr counts, the SDK's and the renderers' atomics). The same instructions.
option(BANJO_SWITCH_OUTLINE_ATOMICS "Switch: GCC's out-of-line atomic helpers (its default; OFF writes them inline)" OFF)
if(REXGLUE_PLATFORM_SWITCH AND NOT BANJO_SWITCH_OUTLINE_ATOMICS)
    add_compile_options(-mno-outline-atomics)
endif()

# renut-ultimate (2026-11-07): the SDK's libraries in each Switch build folder (sdk/CMakeLists.txt), so they can be built
# with each folder's LTO and PGO options (BANJO_SWITCH_SDK_OPT below).
if(REXGLUE_PLATFORM_SWITCH)
    set(REXGLUE_OUTPUT_IN_BUILD ON CACHE BOOL "The SDK's libraries and tools in this build folder")
endif()
# sasrt-nx (2026-11-11, port 71): the SDK's small helpers forced inline (sdk/include/rex/ppc/diet_ops.h,
# REX_SMALL_HELPER). Before the SDK's targets (generated/rexglue.cmake adds them), so the switch reaches them too.
option(SASRT_FORCE_INLINE_HELPERS "The SDK's one-instruction helpers forced inline (OFF: REX_NO_FORCED_HELPERS)" ON)
if(NOT SASRT_FORCE_INLINE_HELPERS)
    add_compile_definitions(REX_NO_FORCED_HELPERS)
endif()
# sasrt-nx (2026-11-11, port 72; its idea 43 F1-F4): VMX's estimates and multiply-add / -subtract in their faster AArch64
# forms (tools/vmx_float_macros.py): vrsqrtefp / vrefp as frsqrte / frecpe with two Newton steps, vlogefp as a NEON log2
# estimate, vmaddfp / vnmsubfp as one fused multiply-add. NOT bit-identical to the exact forms the port computed before:
# sasrt-nx's console fuzz (64 M lanes each) put the estimates within 3 ulp (vrsqrtefp) and 1 ulp (vrefp) of exact and the
# fused forms different in 2.4 % of lanes (one rounding, as the 360's own vmaddfp); the 360's estimates are good to about
# 1/4096. ON at the user's word (2026-11-11). AArch64 only: the PC keeps the exact expressions. Global, so the SDK and
# the copies see the same. An existing build folder keeps a cached OFF: cmake -DSASRT_VMX_FAST_ESTIMATES=ON
# -DSASRT_VMX_FUSED_MADD=ON on it.
option(SASRT_VMX_FAST_ESTIMATES "VMX vrsqrtefp / vrefp / vlogefp as AArch64 estimates (not bit-exact)" ON)
option(SASRT_VMX_FUSED_MADD "VMX vmaddfp / vnmsubfp as one fused multiply-add (not bit-exact)" ON)
if(SASRT_VMX_FAST_ESTIMATES)
    add_compile_definitions(REX_VMX_FAST_ESTIMATES)
endif()
if(SASRT_VMX_FUSED_MADD)
    add_compile_definitions(REX_VMX_FUSED_MADD)
endif()
# blur-nx (2026-11-12, the user: "Please do the fmadds change"): the scalar fmadds / fmsubs / fnmadds / fnmsubs rounded once
# in single precision, as the Xenon does (tools/fma_single.py, sdk/include/rex/ppc/fma_single.h), instead of the double
# fused multiply-add rounded again to single: one unit in the last place apart in rare cases (about one operation in a
# few hundred million), and single-precision chains stay in float registers. OFF: the double rounding, as before.
option(BLUR_FMADDS_SINGLE "Scalar fmadds and kin rounded once in single precision (the Xenon's rounding; not bit-exact to the double form)" ON)
if(NOT BLUR_FMADDS_SINGLE)
    add_compile_definitions(REX_FMADDS_DOUBLE_ROUNDING)
endif()
# renut-ultimate (2026-11-16, port 144; tw2de_nx's fix, off by default at the user's word): fnmadd / fnmsub and their
# single forms with the PowerPC's sign of zero and NaN (sdk/include/rex/ppc/fpu_fma.h, tools/fnm_sign.py's
# REX_FNEG_FUSED). GCC folds the generated -std::fma(...) into FNMADD / FMSUB, which give +0 where the 360 gives -0 and
# negate a NaN result (Xenia's hardware-recorded tests: 160 cases). ON: the fused result kept by an empty asm, then
# negated unless NaN (an instruction or two more a site; Katamari: 272 sites). A compile-time switch: retrain the PGO
# profile.
option(TW2DE_FNMADD_SIGN "fnmadd / fnmsub with the PowerPC's sign of zero and NaN (exact; off by default)" OFF)
if(TW2DE_FNMADD_SIGN)
    add_compile_definitions(REX_FNMADD_SIGN_EXACT)
endif()
# renut-ultimate (2026-11-16, port 144; tw2de_nx's, off by default at the user's word): vsl / vsr with a count per byte,
# as the hardware shifts (rex/ppc/intrinsics.h REX_VSHIFT_PER_BYTE; Xenia's hardware-recorded tests: 453 of 475 vsl
# cases with counts that differ between bytes). The same as before for the ISA's uniform counts; on AArch64 five NEON
# instructions each (the default vsl goes through memory). Katamari: 112 vsr sites. Retrain the PGO profile.
option(TW2DE_VSHIFT_PER_BYTE "vsl / vsr with each byte's own shift count, as the hardware (off by default)" OFF)
if(TW2DE_VSHIFT_PER_BYTE)
    add_compile_definitions(REX_VSHIFT_PER_BYTE)
endif()
# blur-nx (2026-11-12, codegen-ideas-3 CG-13; the user: "build CG 13 ... it probably should be off by default"): all guest
# float code with the host's flush mode on (rex/ppc/context.h, rex/ppc/diet_ops.h): no `msr fpcr` switch in the game code
# after a thread's first, and the any-mode float helpers as the plain operations. NOT bit-exact: the Xenon's scalar FPU
# keeps denormals (values below FLT_MIN / DBL_MIN), which this flushes to zero as its VMX unit does. The PC census
# (banjo_diag_runtime_census: "[census] CG-13" lines, tools/census_sites.py --denormal) counts the results it changes.
# A compile-time switch: every generated file, the copies and the SDK runtime change with it (retrain the PGO profile).
# renut-ultimate (2026-11-13, port 133; the user: "turn it on by default in the base ... because it's universal big CPU
# benefit"): ON by default in the Switch build (each switch is an `msr fpcr` on the console: Katamari's game code does
# about 1.2 million a second on the PC; blur-nx's console +4.6 % fps). OFF by default in the PC build, which stays
# exact: the census counts what the Switch build changes only where the mode still switches
# (tools/cg13_census.ps1). An existing build folder keeps its cached value: -DBLUR_FPCR_ONE_MODE=ON / OFF changes it.
if(REXGLUE_PLATFORM_SWITCH)
    set(_blur_fpcr_one_mode_default ON)
else()
    set(_blur_fpcr_one_mode_default OFF)
endif()
option(BLUR_FPCR_ONE_MODE "All guest float code in the flush mode (no FPCR switches; not bit-exact for scalar denormals)"
       ${_blur_fpcr_one_mode_default})
if(BLUR_FPCR_ONE_MODE)
    add_compile_definitions(REX_FPCR_ONE_MODE)
endif()
# renut-ultimate (2026-11-14, port 140; blur-nx's docs/cpu-near-exact-50-1113.md): the compile-time near-exact switches
# (rex/ppc/diet_ops.h, context.h, tools/near_exact.py), each the code before when off. NOT bit-exact in rare cases:
# #1 REX_NX_SNAN_PASS, #2 REX_NX_FCTI_NO_NAN, #17 REX_NX_DIV_NO_CHECK, #41 REX_NX_FCMP_NO_UN, #42 REX_NX_VMINMAX_FAST,
# #5 REX_NX_XER_SO_ZERO, #19 REX_NX_NO_TRAPS (a trap only logs). OFF in the base: each game runs the PC census
# (banjo_diag_runtime_census, the "[census] near-exact" lines) before turning them on. blur-nx's census took #2 (NaN
# conversions not rare: 80 a second) and #42 (4 % of vector max/min lanes differ) out of the set; BLUR_NX_ONLY names
# switches to use alone (a ;-list, for a bisect; #2 and #42 only this way), BLUR_NX_SKIP leaves some out. Every
# generated file, the copies and the SDK change with them: retrain the PGO profile.
option(BLUR_NEAR_EXACT "blur-nx's near-exact compile-time switches (not bit-exact in rare cases)" OFF)
set(BLUR_NX_ONLY "" CACHE STRING "With BLUR_NEAR_EXACT: only these switches (a ;-list, for a bisect; empty: all)")
set(BLUR_NX_SKIP "" CACHE STRING "With BLUR_NEAR_EXACT: every switch but these (a ;-list)")
set(_blur_nx_switches REX_NX_SNAN_PASS REX_NX_DIV_NO_CHECK REX_NX_FCMP_NO_UN REX_NX_XER_SO_ZERO REX_NX_NO_TRAPS)
if(BLUR_NEAR_EXACT AND NOT BLUR_NX_ONLY STREQUAL "")
    list(APPEND _blur_nx_switches REX_NX_FCTI_NO_NAN REX_NX_VMINMAX_FAST)
endif()
if(BLUR_NEAR_EXACT)
    foreach(_s ${_blur_nx_switches})
        if((BLUR_NX_ONLY STREQUAL "" OR _s IN_LIST BLUR_NX_ONLY) AND NOT _s IN_LIST BLUR_NX_SKIP)
            add_compile_definitions(${_s})
            message(STATUS "near-exact: ${_s}")
        endif()
    endforeach()
endif()
# renut-ultimate (2026-11-14, port 140; blur-nx's #66, exact): the generated tree's store cards by the copies' rule
# (tools/store_cards.py: only the pages whose cards Renut reads, a card written only when not set yet). ON, as in
# blur-nx's every build; OFF: every store marks its card as before.
option(BLUR_CARD_VIEWS_ONLY "The generated tree's store cards only for Renut's views, written only when set (exact)" ON)
if(BLUR_CARD_VIEWS_ONLY)
    add_compile_definitions(REX_CARD_VIEWS_ONLY)
endif()
include(generated/rexglue.cmake)
# Port 158 (from fnxf_xiii, its note of 2026-10-26): the function registration (generated/renut_register.cpp, one
# function of about 44,000 SetFunction calls, run once at start) without optimisation in the Switch build. GCC's
# optimisation of it was the build's critical path: 1,171 s of a 1,195 s full Switch build (Katamari, 2026-11-18;
# fnxf_xiii: 81 minutes at -O2). Appended to the generated sources' options, so it comes after their -O level and
# wins. Without the precompiled header: GCC does not use a PCH made with optimisation in an -O0 file ("__OPTIMIZE__ not
# defined"); renut_init.h includes renut_pch.h itself. LTO keeps each function's own level. OFF: as before.
option(BANJO_REGISTER_O0 "Switch: the function registration file at -O0, without the PCH (build time)" ON)
if(BANJO_REGISTER_O0 AND REXGLUE_PLATFORM_SWITCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/renut_register.cpp")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/renut_register.cpp" APPEND PROPERTY COMPILE_OPTIONS -O0)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/renut_register.cpp" PROPERTY SKIP_PRECOMPILE_HEADERS ON)
endif()

# banjo-nx (from sa2-nx): the optimised codegen. The build re-runs the generator by itself whenever an input in
# codegen.d changes (renut_config.toml with the codegen keys, the xex, the generator binary). That rewrites
# app/generated and would silently drop the post-codegen steps (no-volatile pch + loop barriers, direct calls): they are
# appended to the same command (docs/enhancements.md, "Build-time"). The Switch cross build has no generator target.
if(TARGET renut_codegen)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen.build.stamp" APPEND
        COMMAND "${CMAKE_COMMAND}" -E env "REXSDK_DIR=${REXSDK_DIR}"
                "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/post_codegen.py")
endif()
# The codegen flavour (written by tools/post_codegen.py) names the build in the "[enhancements]" log line.
set(BANJO_FLAVOR_FILE "${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen_flavor.txt")
set(BANJO_BUILD_FLAVOR "classic codegen")
if(EXISTS "${BANJO_FLAVOR_FILE}")
    file(READ "${BANJO_FLAVOR_FILE}" BANJO_BUILD_FLAVOR)
    string(STRIP "${BANJO_BUILD_FLAVOR}" BANJO_BUILD_FLAVOR)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BANJO_FLAVOR_FILE}")
endif()
set_source_files_properties(src/cpu/banjo_enhancements.cpp PROPERTIES
    COMPILE_DEFINITIONS "BANJO_BUILD_FLAVOR=\"${BANJO_BUILD_FLAVOR}\"")
# The native game-function replacements reproduce the generated code's host flush mode at exit; with the codegen option
# fp_moves_modeless (renut_config.toml) a floating-point move no longer switches it (src/cpu/banjo_reverb_native.cpp).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/renut_config.toml" BANJO_FP_MOVES_MODELESS_LINE REGEX "^fp_moves_modeless *= *true")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/renut_config.toml")
if(BANJO_FP_MOVES_MODELESS_LINE)
    set_source_files_properties(src/cpu/banjo_reverb_native.cpp PROPERTIES COMPILE_DEFINITIONS "BANJO_FP_MOVES_MODELESS=1")
endif()



# katamari-nx (2026-10-30): banjo-nx's app without the files written for N&B's addresses (its cheats and mid-asm
# hooks, title update and DLC, the D3D hook layer of banjo-nx's PM4 renderer, its XAudio and hand-written natives, the
# shader harvest): src/katamari_keys.cpp defines the keys of those files that the kept ones still read.
set(RENUT_SOURCES
    src/main.cpp
    src/katamari_keys.cpp
    src/game_menu.cpp  # blur-nx (2026-11-18): the game menu (A+B+X+Y+L+R on the Switch, F8 on the PC)
    src/blur_small_alloc.cpp  # blur-nx (2026-11-17, #220; port 151): small C++ allocations per thread (Switch; 32 MB)
    # katamari-nx (2026-10-31): DLC packages in the "dlc" folder next to the NRO installed at start (reNut upstream's).
    src/renut_engine/dlc_install.cpp
	src/renut_engine/FPS.cpp
    # CPU/GPU profiler ported from UnleashedRecomp-NX (see src/profiler/profiler.h).
    src/profiler/profiler.cpp
    # Native renderer (from masseffect-nx, itself derived from nfsmw-nx): reads the game's PM4 ring and draws with
    # Vulkan instead of the emulated Xenos GPU. On with masseffect_renderer_native = true. docs/native-renderer-plan.md.
    src/me/me_native_system.cpp
    src/me/me_ring_wait.cpp
    src/me/masseffect/masseffect_native_draws.cpp
    src/me/masseffect/masseffect_deferred_recording.cpp
    src/me/masseffect/masseffect_native_targets.cpp
    src/me/masseffect/masseffect_native_shaders.cpp
    src/me/masseffect/masseffect_native_textures_pool.cpp
    src/me/masseffect/masseffect_shader_library.cpp
    src/me/masseffect/me_masseffect_glue.cpp
    # katamari-nx: banjo-nx's PM4 renderer is compiled (the enhancement table reads its keys) but its D3D hook layer
    # (src/me/banjo_me_hooks.cpp, N&B's addresses) is not: masseffect_renderer_native stays false.
    src/me/banjo_renderdoc.cpp
    # On/off switches and the startup summary of every performance enhancement (src/cpu/banjo_enhancements.h).
    src/cpu/banjo_enhancements.cpp
    # sa2-nx's CPU rounds: the PC side of the post-codegen self-checks and the censuses (empty on the Switch), and
    # the start-up check of every instruction form that only one processor has.
    src/cpu/banjo_codegen_checks.cpp
    src/cpu/banjo_forms_selfcheck.cpp
    # blur-nx (2026-11-11, port 90): timings of a few processor forms at boot (blur_microbench, off).
    src/cpu/blur_microbench.cpp
    # katamari-nx (2026-11-01): diagnostic of the game state's stage fields (katamari_diag_game_state, off).
    src/cpu/katamari_stage_diag.cpp
    # katamari-nx: the native copies of hot game functions (banjo_hot_guest.cpp and its shards), from this game's PC
    # profile (docs/katamari-notes.md, "Hot functions"): src/cpu/hot/localized.txt and hand.txt.
    src/cpu/banjo_hot_guest.cpp
    src/cpu/banjo_hot_shard_0.cpp
    src/cpu/banjo_hot_shard_1.cpp
    src/cpu/banjo_hot_shard_2.cpp
    src/cpu/banjo_hot_shard_3.cpp
    src/cpu/banjo_hot_shard_4.cpp
    src/cpu/banjo_hot_shard_5.cpp
    src/cpu/banjo_hot_shard_6.cpp
    src/cpu/banjo_hot_shard_7.cpp
    src/cpu/banjo_hot_shard_8.cpp
    src/cpu/banjo_hot_shard_9.cpp
    src/cpu/banjo_hot_shard_10.cpp
    src/cpu/banjo_hot_shard_11.cpp
    src/profiler/banjo_pc_profiler.cpp
    # PNG captures of the presented game image (nfsmw_captura_cada_s), from nfsmw-nx: for testing.
    src/native/nfsmw_nativo_captura.cpp
)

# Keyboard + mouse needs a desktop window; the Switch gets no-op hooks instead.
if(REXGLUE_PLATFORM_SWITCH)
    list(APPEND RENUT_SOURCES
        src/renut_engine/mnk_controls_switch.cpp
        # Unleashed's sampling CPU profiler and stall watchdog (libnx only).
        src/profiler/cpu_profiler_switch.cpp
        src/profiler/stall_watch_switch.cpp
        # PGO profile collection (BANJO_PGO=generate; empty otherwise). From sa2-nx.
        src/banjo_pgo.cpp)
else()
    list(APPEND RENUT_SOURCES src/renut_engine/mnk_controls.cpp
        # Switch profiler counters the native renderer calls (no-ops on PC).
        src/me/me_pc_stubs.cpp
        # Sampling CPU profiler for PC tests, from nfsmw-nx (nfsmw_perfil_pc_desde_s; logs/perfil_pc.csv).
        src/profiler/nfsmw_perfil_pc.cpp)
endif()

if(WIN32)
list(APPEND RENUT_SOURCES icon/app.rc)
    add_executable(renut WIN32 ${RENUT_SOURCES})
else()
    add_executable(renut ${RENUT_SOURCES})
endif()

# GPU_PLUGINS stages the emulated-GPU backend (rexgpu-xenos.dll) next to the exe.
# It is only used when the gpu_plugin cvar is "xenos"; empty means no GPU at all
# (the game runs but the screen stays black).
rexglue_setup_target(renut GPU_PLUGINS xenos)

# banjo-nx (2026-10-17): Ryujinx starts NROs without argv[0]; the SDK then takes the NRO's usual place on the SD
# (sdk/src/core/filesystem_posix.cpp). A console's hbloader always passes the path, so nothing changes there.
if(REXGLUE_PLATFORM_SWITCH AND TARGET rexcore)
    target_compile_definitions(rexcore PRIVATE REX_SWITCH_FALLBACK_EXE="/switch/katamari-nx/katamari-nx.nro")
endif()

# banjo-nx (2026-10-16): Renut's Direct3D-level renderer as a second renderer, chosen at boot (banjo_renderer = "renut";
# cmake/banjo_renut_gpu.cmake, docs/renut-renderer-port.md). OFF builds banjo-nx's own renderer only.
option(BANJO_RENUT_RENDERER "Build Renut's native renderer in (banjo_renderer = renut)" ON)
if(BANJO_RENUT_RENDERER)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/banjo_renut_gpu.cmake")
endif()

# The SDK compiles its own targets with these (sdk/CMakeLists.txt) but they did not reach the app's: without them GCC
# (Switch) fuses separate guest multiplies and adds into FMAs across statements (-ffp-contract=fast is its default),
# so the recompiled game's floating point drifted from the Xbox 360 and from the PC build (x86-64-v2 has no FMA), and
# type-punned guest register/memory accesses relied on strict aliasing. The native game-function replacements are
# checked against the recompiled code built this way.
foreach(_banjo_target renut renut_recomp)
    if(TARGET ${_banjo_target})
        target_compile_options(${_banjo_target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
    endif()
endforeach()

# banjo-nx (masseffect-nx ships these, futari-nx measured them): the generated game code without errno bookkeeping in
# math functions (sqrt and friends become one instruction instead of a possible libm call) and without assuming a
# floating-point operation can trap. IEEE results are unchanged (no -ffinite-math-only: NaN and infinity checks stay);
# only errno and trap semantics, which the recompiled code never observes. Matters on the Switch (GCC's defaults are
# -fmath-errno -ftrapping-math); clang for the PC mostly assumes them already. -DBANJO_GENERATED_MATH_FLAGS=OFF to drop.
# 2026-10-12 (sa2-nx's audit): only -fno-math-errno. -fno-trapping-math lets GCC run a float operation earlier than the
# code does (out of a branch or a loop), and the recompiled code switches the host's flush mode in between (msr fpcr),
# which the compiler does not know a float operation depends on: an operation moved in front of a mode check would
# compute a denormal operand in the wrong mode. It bought sa2-nx at most 0.5 % of one file. Not exact, so off;
# -DBANJO_GENERATED_NO_TRAPPING_MATH=ON puts it back for a measurement.
option(BANJO_GENERATED_MATH_FLAGS "-fno-math-errno on the generated game code" ON)
option(BANJO_GENERATED_NO_TRAPPING_MATH "-fno-trapping-math on the generated game code (not exact, see the comment)" OFF)
if(BANJO_GENERATED_MATH_FLAGS AND TARGET renut_recomp AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(renut_recomp PRIVATE -fno-math-errno)
    if(BANJO_GENERATED_NO_TRAPPING_MATH)
        target_compile_options(renut_recomp PRIVATE -fno-trapping-math)
    endif()
endif()

# The localized native copies (src/cpu/banjo_hot_guest.cpp includes generated/hot) are game code compiled in the app:
# the same rules as renut_recomp for that one file (sa2-nx's findings). -fno-math-errno: GCC does not inline between
# two functions that compute with floats and differ in it, and the guard compares a copy with code built that way.
# -ffp-contract=off is already on the whole target (above). On the PC the same census definition as the game code, so
# the any-mode helper counts stay complete.
# 2026-10-22: the bulk shards are the same kind of code (BANJO_HOT_SOURCES: every file that compiles copies).
set(BANJO_HOT_SOURCES src/cpu/banjo_hot_guest.cpp
    src/cpu/banjo_hot_shard_0.cpp
    src/cpu/banjo_hot_shard_1.cpp
    src/cpu/banjo_hot_shard_2.cpp
    src/cpu/banjo_hot_shard_3.cpp
    src/cpu/banjo_hot_shard_4.cpp
    src/cpu/banjo_hot_shard_5.cpp
    src/cpu/banjo_hot_shard_6.cpp
    src/cpu/banjo_hot_shard_7.cpp
    src/cpu/banjo_hot_shard_8.cpp
    src/cpu/banjo_hot_shard_9.cpp
    src/cpu/banjo_hot_shard_10.cpp
    src/cpu/banjo_hot_shard_11.cpp
)
if(BANJO_GENERATED_MATH_FLAGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_property(SOURCE ${BANJO_HOT_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -fno-math-errno)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH)
    set_property(SOURCE ${BANJO_HOT_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS REX_DIET_CENSUS_ON)
endif()
# sasrt-nx (2026-11-11, port 74; its machine-copy ideas 57, 47, 52 and 39): switches of the localized copies
# (tools/localize_native.py), each ON as in sasrt-nx; OFF compiles the copies as before port 74. The copies' code
# changes with them: retrain the PGO profile (docs/building.md; ninja does not track the .gcda files: build the use step
# clean). The generator's own switches are environment variables at post-codegen (SASRT_LOCALIZE_*, see the tool).
#   - SASRT_HOT_MERGED_STORES (idea 57): an stvlx / stvrx pair, or four stvewx of one vector, as one 16-byte store of
#     the same bytes in the copies' running form when the addresses are adjacent at run time. OFF: the original stores.
#   - SASRT_HOT_LR_ELIDE (idea 47): the 12 bulk shards (not banjo_hot_guest.cpp's hand-picked copies, not the generated
#     tree) compiled with tools/frame_traffic.py's opt-in REX_LR_MOVES_ELIDE: REX_LR_READ gives 0, the link register's
#     save and REX_LR_WRITE are nothing (with C++ returns the value is never used; frame_traffic.py marks only
#     functions where it is not data). The dead word at -8 of a copy's frame keeps its old content (sa2-nx: the link
#     register's half of the three elisions passed its frame-exact capture). The guard compares two forms that both
#     elide. The hand-written natives included by the shards that test REX_LR_MOVES_ELIDE (the XAudio filter's frame,
#     cpu/hot/banjo_hand_natives.h) elide there too. BANJO_GEN_LR_MOVES_ELIDE (below) is the same for all game code.
#   - SASRT_HOT_PREFETCH (idea 52): REX_PREFETCH_GUEST after a pointer-chasing loop's next-pointer load. OFF: nothing.
#   - SASRT_HOT_CARD_RUNS (idea 39): the 12 bulk shards mark the store cards per run of stores to a page (cpu/
#     banjo_hot_prelude.h; only for copies written with the runs). OFF: at every store, as the generated tree does.
option(SASRT_HOT_MERGED_STORES "Localized copies: merged unaligned vector stores (tools/localize_native.py)" ON)
# renut-ultimate (2026-11-11, port 84, the user's call): OFF by default. vivapinata-nx's guard found 8 differences from it
# (the link register's save elided in one of a copy's forms and not the other: forms in different translation units,
# only one with the define). ON in a game that has checked its copies with it (a guard run, 0 differences).
option(SASRT_HOT_LR_ELIDE "The bulk copies (shards 0-11): the link register's dead moves left out (REX_LR_MOVES_ELIDE)" OFF)
option(SASRT_HOT_PREFETCH "Localized copies: a prefetch after a loop's next-pointer load (REX_PREFETCH_GUEST)" ON)
option(SASRT_HOT_CARD_RUNS "The bulk copies (shards 0-11): store cards marked per run of stores to a page" ON)
if(NOT SASRT_HOT_MERGED_STORES)
    target_compile_definitions(renut PRIVATE SASRT_HOT_MERGED_STORES_OFF)
endif()
if(NOT SASRT_HOT_PREFETCH)
    target_compile_definitions(renut PRIVATE SASRT_HOT_PREFETCH_OFF)
endif()
if(SASRT_HOT_LR_ELIDE)
    foreach(_shard RANGE 0 11)
        set_property(SOURCE src/cpu/banjo_hot_shard_${_shard}.cpp APPEND PROPERTY COMPILE_DEFINITIONS REX_LR_MOVES_ELIDE)
    endforeach()
endif()
if(SASRT_HOT_CARD_RUNS)
    foreach(_shard RANGE 0 11)
        set_property(SOURCE src/cpu/banjo_hot_shard_${_shard}.cpp APPEND PROPERTY COMPILE_DEFINITIONS SASRT_HOT_CARD_RUNS)
    endforeach()
endif()
# sasrt-nx (2026-11-10, port 78; its idea 27): the guard's two logged forms of every localized copy (the copy's
# logged form and the original statements' o_* form: about two thirds of the copies' code; sasrt-nx's NRO +9.3 MB,
# +12 %) are made only with SASRT_HOT_GUARD_FORMS. Without them (BANJO_HOT_GUARD_FORMS=0, cpu/banjo_hot_guest.h) every hook calls its
# copy's running form directly, whatever banjo_cpu_verify and banjo_hot_guard_* say, and the log says once that the
# build has no guard forms. sasrt-nx's default is ON, OFF in its -Final build; here it is per platform:
#   - Switch OFF: the everyday build (app/out/switch), the release and PGO folders run no guard (the compiled-in
#     settings set banjo_hot_guard_calls = banjo_hot_guard_period = 0). An instrumented PGO folder gets the same default
#     as its use folder, so the profile is taken on the same code. A Switch NRO for a guard or verify run (Ryujinx or the
#     console with a guard toml): tools/build_switch_guard.bat (app/out/switch-guard).
#   - PC ON: tools/run_pc_verify.ps1 and the guard keys' own defaults (64 / 4096) need the forms.
# An existing folder keeps its cached value (cmake -DSASRT_HOT_GUARD_FORMS=ON|OFF <folder>; build_switch.bat's
# GUARD_FORMS). The running forms' code is the same either way. The definition goes on the files that compile copies
# (BANJO_HOT_SOURCES; sasrt-nx puts it on its whole target), so a change compiles only those again.
if(REXGLUE_PLATFORM_SWITCH)
    set(_sasrt_hot_guard_forms_default OFF)
else()
    set(_sasrt_hot_guard_forms_default ON)
endif()
option(SASRT_HOT_GUARD_FORMS "Localized copies: the guard's logged forms (banjo_cpu_verify, banjo_hot_guard_*); default ON on the PC, OFF on the Switch" ${_sasrt_hot_guard_forms_default})
if(NOT SASRT_HOT_GUARD_FORMS)
    set_property(SOURCE ${BANJO_HOT_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS BANJO_HOT_GUARD_FORMS=0)
endif()
message(STATUS "renut: localized copies' guard forms ${SASRT_HOT_GUARD_FORMS} (SASRT_HOT_GUARD_FORMS)")
# sasrt-nx (2026-11-11, port 81; machine copies in place): sasrt-nx's docs/handoff/inplace_copies.md.
# With SASRT_HOT_IN_PLACE every localized copy's hook is a bare redirect (BANJO_HOT_IN_PLACE=1, cpu/banjo_hot_guest.h):
# the generated function's symbol is the copy's Native, which loses its [[gnu::noinline]] (BANJO_HOT_NATIVE_SPEC,
# written by tools/localize_native.py), so the compiler and LTO may inline a copy into its callers, and the generated
# original loses its last reference (--gc-sections and LTO drop it). No state byte, no guard (it implies no guard
# forms), no run-time key, no in-run A/B 70: a copy its list marks `off` keeps the original, every other one runs, and
# the start-up log names each key set against that (banjo_hot_guest.cpp, InPlaceNotes). The hand-written natives keep
# their keys. sasrt-nx's console race, each form with its own PGO + LTO: game thread -4.1 %, process CPU -2.0 %, NRO
# -4.1 MB. The lists and the copies' code are the same in both forms, so a hooked guard build (tools/
# build_switch_guard.bat, the PC's verify build) checks the in-place code.
# Default (the user: every optimisation on): ON on the Switch wherever the guard forms are OFF (the everyday, release
# and PGO folders), OFF otherwise (the PC verify build, app/out/switch-guard: a guard needs its hooks). An existing
# folder takes the default at its next configure and keeps it (cmake -DSASRT_HOT_IN_PLACE=ON|OFF <folder>;
# build_switch.bat's IN_PLACE). The two forms compile to different code: each has its own PGO profile
# (BANJO_PGO_DIR, BANJO_PGO_DIR_IN_PLACE below). sasrt-nx's list option `inplace_only` (small copies compiled in place
# only) is not taken: sasrt-nx looked at it and dropped it.
if(REXGLUE_PLATFORM_SWITCH AND NOT SASRT_HOT_GUARD_FORMS)
    set(_sasrt_hot_in_place_default ON)
else()
    set(_sasrt_hot_in_place_default OFF)
endif()
option(SASRT_HOT_IN_PLACE "Localized copies in place of the generated functions (no hooks, no guard, no per-copy keys); default ON on the Switch without guard forms" ${_sasrt_hot_in_place_default})
if(SASRT_HOT_IN_PLACE)
    set_property(SOURCE ${BANJO_HOT_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS BANJO_HOT_IN_PLACE=1)
    if(SASRT_HOT_GUARD_FORMS)
        set_property(SOURCE ${BANJO_HOT_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS BANJO_HOT_GUARD_FORMS=0)
        message(WARNING "renut: SASRT_HOT_IN_PLACE ON has no hooks and so no guard: SASRT_HOT_GUARD_FORMS ON is not "
                        "used (a guard or verify build: -DSASRT_HOT_IN_PLACE=OFF, tools/build_switch_guard.bat)")
    endif()
endif()
message(STATUS "renut: localized copies in place ${SASRT_HOT_IN_PLACE} (SASRT_HOT_IN_PLACE)")

# sa2-nx's CPU rounds of 2026-10-12 (docs/enhancements.md, "sa2-nx's CPU rounds"): one switch back per post-codegen
# step (tools/post_codegen.py). OFF compiles the statements as they were before the step, on every platform, so a PC
# build can take one step out to find which of them a difference comes from.
option(BANJO_GEN_RO_CONSTANTS "Loads from the image's read-only data as constants" ON)
option(BANJO_GEN_FLOAT_LOADS "Single-precision loads straight into a float register" ON)
# The three stack-frame elisions of tools/frame_traffic.py are OFF: they change what stale data the guest's stack
# frames hold; sa2-nx found a game that reads a frame word it never wrote (characters tinted). NOT exact.
option(BANJO_GEN_LR_MOVES_ELIDE "No moves of the guest link register in prologues and epilogues (NOT exact)" OFF)
option(BANJO_GEN_NV_SAVES_ELIDE "No prologue saves of non-volatile registers that are locals (NOT exact)" OFF)
option(BANJO_GEN_BACK_CHAIN_ELIDE "No back-chain stores of the stack frames (NOT exact for a guest stack walk)" OFF)
option(BANJO_GEN_LEAF_LOCALS "ctx registers of leaf functions with loops as locals" ON)
option(BANJO_GEN_LOOP_LOCALS "ctx registers of call-free loops as locals for the length of the loop" ON)
option(BANJO_GEN_COLD_TRAPS "The trap helper out of line and cold" ON)
option(BANJO_GEN_PROMOTED_CALLS "Guarded direct calls at the promoted indirect call sites" ON)
# From sasrt-nx's codegen round of 2026-10-14 (tools/*.py named below; each tested in sdk/tests).
option(BANJO_GEN_VPERM1 "A vperm of one vector as a one-register lookup (tools/vperm_one_source.py)" ON)
option(BANJO_GEN_FP_SIGN_OPS "fneg / fabs / fnabs as float operations (tools/fp_sign_ops.py)" ON)
option(BANJO_GEN_LEAF_DEAD "Leaf write-backs no caller can read are skipped (tools/leaf_dead_stores.py)" ON)
# The PC test of BANJO_GEN_LEAF_DEAD: a skipped register gets a poison value instead (0xDEADBEEF..., a signalling NaN),
# so a reader the analysis missed breaks the game visibly. Never for a build that ships.
option(BANJO_GEN_LEAF_DEAD_POISON "PC test: poison the skipped leaf write-backs" OFF)
option(BANJO_GEN_DEAD_DEFS "Guest register writes no instruction can read are left out (tools/dead_ctx_defs.py)" ON)
# The PC test of BANJO_GEN_DEAD_DEFS: the register gets a poison value instead. Never for a build that ships.
option(BANJO_GEN_DEAD_DEFS_POISON "PC test: poison the left-out register writes" OFF)
option(BANJO_GEN_SINGLE_FLOATS "Single-precision arithmetic in float where its inputs are singles (tools/single_floats.py)" ON)
option(BANJO_GEN_INLINE_MEMCALLS "memcpy / memset of a constant size up to 64 bytes inline (tools/inline_memcalls.py)" ON)
option(BANJO_GEN_VEC_UNALIGNED "stvlx / stvrx as a few stores instead of byte loops (tools/vec_unaligned_stores.py)" ON)
option(BANJO_GEN_NARROW_LOOP_BARRIER "Loop barriers clobber the guest window only, not all memory (tools/loop_barriers.py, from marathon-nx)" ON)
option(BANJO_GEN_DCBT_PREFETCH "The dcbt / dcbtst hints as host prefetches (generator dcbt_prefetch; OFF: nothing)" ON)
# 2026-10-16 (CPU codegen agent): the vector registers of the loop and leaf locals (tools/loop_locals.py,
# tools/leaf_locals.py) as values instead of references to ctx, so a vector result is not stored to ctx on every loop
# iteration. Measured on all 213 files: -1.42 % weighted by the console's samples, +0.05 % instructions (294 of the 481
# functions it touches cheaper, 54 dearer; the XAudio mixers -15 to -19 %). Safe with -mearly-ra=none (the cause of
# peggle2-nx's fault of 2026-10-13, see loop_locals.py).
option(BANJO_GEN_VECTOR_LOCALS_BY_VALUE "Vector registers of the loop and leaf locals as values, not ctx references" ON)
# 2026-10-16 (CPU codegen agent): read-only constants found over the control-flow graph (tools/ro_constants_cfg.py):
# 2,730 more loads folded, mostly lfs in loops. The PC build compares each with the real load (RexRoCheck).
option(BANJO_GEN_RO_CFG "Read-only constants followed over the control-flow graph (tools/ro_constants_cfg.py)" ON)
# 2026-10-16 (the CPU codegen agent's aggressive round, docs/codegen-enhancements.md steps 23 and 25-29): each has its switch
# back here; the PC build checks the r1 mirror and the redundant link-register write-backs (REX_R1_CHECK,
# REX_LR_WRITE_SAME_CHECK, reported in the [verify] line of banjo_codegen_checks.cpp).
option(BANJO_GEN_FLUSH_HOIST "Loop-header flush-mode checks in front of the loop (tools/flush_hoist.py)" ON)
option(BANJO_HOT_LAZY_LOG10 "The XAudio limiter's localized copy computes log10 only where it is read (tools/limiter_lazy_log10.py)" ON)
option(BANJO_GEN_SWITCH_THREAD "The script interpreters' jump tables as threaded computed gotos (tools/switch_goto.py)" ON)
option(BANJO_GEN_LR_WRITE_SAME "Link-register write-backs proven redundant compile to nothing (tools/lr_preserve.py)" ON)
option(BANJO_GEN_R1_LOCAL "Guest r1 as a local mirror in the chosen functions (tools/r1_local.py)" ON)
# Package 27's codegen round (docs/codegen-enhancements.md): each a post-codegen step with its switch back.
option(BANJO_GEN_VSEL_NEON "VMX vsel as one NEON bit select (tools/vsel_neon.py)" ON)
option(BANJO_GEN_HOT_LAYOUT "The console profile's functions declared hot, gathered by the linker (tools/hot_layout.py)" ON)
option(BANJO_GEN_INLINE_LEAVES "Inline copies of the tiny leaf functions each file calls in another (tools/inline_leaves.py)" ON)
# 2026-10-19 (CPU proposal 1): every guest store outside the stack marks its page's store card (tools/store_cards.py);
# Renut proves its cached vertex and index copies by them (banjo_renut_store_cards). OFF compiles the marks out (Renut
# then hashes as before: app/src/cpu/banjo_hot_guest.cpp tells it).
option(BANJO_GEN_STORE_CARDS "Guest stores outside the stack mark their page's store card (tools/store_cards.py)" ON)
set(_banjo_round_definitions "")
if(NOT BANJO_GEN_VPERM1)
    list(APPEND _banjo_round_definitions REX_VPERM1_OFF)
endif()
if(NOT BANJO_GEN_FP_SIGN_OPS)
    list(APPEND _banjo_round_definitions REX_FP_SIGN_INTEGER)
endif()
if(NOT BANJO_GEN_LEAF_DEAD)
    list(APPEND _banjo_round_definitions REX_LEAF_DEAD_KEEP)
endif()
if(BANJO_GEN_LEAF_DEAD_POISON)
    list(APPEND _banjo_round_definitions REX_LEAF_DEAD_POISON)
endif()
if(NOT BANJO_GEN_DEAD_DEFS)
    list(APPEND _banjo_round_definitions REX_DEAD_DEFS_KEEP)
endif()
if(BANJO_GEN_DEAD_DEFS_POISON)
    list(APPEND _banjo_round_definitions REX_DEAD_DEFS_POISON)
endif()
if(NOT BANJO_GEN_SINGLE_FLOATS)
    list(APPEND _banjo_round_definitions REX_SINGLE_FLOATS_OFF)
endif()
if(NOT BANJO_GEN_INLINE_MEMCALLS)
    list(APPEND _banjo_round_definitions REX_INLINE_MEMCALLS_OFF)
endif()
if(NOT BANJO_GEN_VEC_UNALIGNED)
    list(APPEND _banjo_round_definitions REX_VEC_UNALIGNED_LOOPS)
endif()
if(NOT BANJO_GEN_RO_CONSTANTS)
    list(APPEND _banjo_round_definitions REX_RO_CONSTANTS_OFF)
endif()
if(NOT BANJO_GEN_FLOAT_LOADS)
    list(APPEND _banjo_round_definitions REX_FLOAT_LOADS_INTEGER)
endif()
if(BANJO_GEN_LR_MOVES_ELIDE)
    list(APPEND _banjo_round_definitions REX_LR_MOVES_ELIDE)
endif()
if(BANJO_GEN_NV_SAVES_ELIDE)
    list(APPEND _banjo_round_definitions REX_NV_SAVES_ELIDE)
endif()
if(BANJO_GEN_BACK_CHAIN_ELIDE)
    list(APPEND _banjo_round_definitions REX_BACK_CHAIN_ELIDE)
endif()
if(NOT BANJO_GEN_LOOP_LOCALS)
    list(APPEND _banjo_round_definitions REX_LOOP_LOCALS_OFF)
endif()
if(NOT BANJO_GEN_LEAF_LOCALS)
    list(APPEND _banjo_round_definitions REX_LEAF_LOCALS_OFF)
endif()
if(NOT BANJO_GEN_COLD_TRAPS)
    list(APPEND _banjo_round_definitions "REX_TRAP_ATTR=")
endif()
if(NOT BANJO_GEN_PROMOTED_CALLS)
    list(APPEND _banjo_round_definitions REX_PROMOTED_CALLS_OFF)
endif()
# sasrt-nx (2026-11-12, its idea 2; port 121): the hooked builds (the guard's: the PC, Ryujinx, tools/build_switch_guard.bat)
# check each forwarded stack load against the real one (tools/stack_forward.py, REX_STACK_FWD_VERIFY): the guard compares
# copies with the original statements, which forward the same way, so it cannot see a wrong forward. The in-place
# console builds forward only.
if(NOT SASRT_HOT_IN_PLACE)
    list(APPEND _banjo_round_definitions REX_STACK_FWD_VERIFY)
endif()
if(NOT BANJO_GEN_NARROW_LOOP_BARRIER)
    list(APPEND _banjo_round_definitions REX_LOOP_BARRIER_FULL)
endif()
if(NOT BANJO_GEN_DCBT_PREFETCH)
    list(APPEND _banjo_round_definitions "REX_PREFETCH(x,write)=")
endif()
if(BANJO_GEN_VECTOR_LOCALS_BY_VALUE)
    list(APPEND _banjo_round_definitions REX_VECTOR_LOCALS_BY_VALUE)
endif()
if(NOT BANJO_GEN_RO_CFG)
    list(APPEND _banjo_round_definitions REX_RO_CFG_OFF)
endif()
if(NOT BANJO_GEN_FLUSH_HOIST)
    list(APPEND _banjo_round_definitions REX_FLUSH_HOIST_OFF)
endif()
if(NOT BANJO_HOT_LAZY_LOG10)
    list(APPEND _banjo_round_definitions BANJO_LAZY_LOG10_ON=0)
endif()
if(NOT BANJO_GEN_SWITCH_THREAD)
    list(APPEND _banjo_round_definitions REX_SWITCH_THREAD_OFF)
endif()
if(NOT BANJO_GEN_LR_WRITE_SAME)
    list(APPEND _banjo_round_definitions REX_LR_WRITE_SAME_OFF)
endif()
if(NOT BANJO_GEN_R1_LOCAL)
    list(APPEND _banjo_round_definitions REX_R1_LOCAL_OFF)
endif()
if(NOT BANJO_GEN_VSEL_NEON)
    list(APPEND _banjo_round_definitions REX_VSEL_OFF)
endif()
if(NOT BANJO_GEN_HOT_LAYOUT)
    list(APPEND _banjo_round_definitions REX_HOT_LAYOUT_OFF)
endif()
if(NOT BANJO_GEN_INLINE_LEAVES)
    list(APPEND _banjo_round_definitions REX_INLINE_LEAVES_OFF)
endif()
if(NOT BANJO_GEN_STORE_CARDS)
    list(APPEND _banjo_round_definitions REX_STORE_CARDS_OFF)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH)
    list(APPEND _banjo_round_definitions REX_R1_CHECK REX_LR_WRITE_SAME_CHECK)
endif()
# PC builds of the game code count the executions of the any-mode float helpers (rex/ppc/diet_ops.h), for
# banjo_diag_runtime_census; nothing on the Switch.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renut_recomp)
    target_compile_definitions(renut_recomp PRIVATE REX_DIET_CENSUS_ON)
endif()
if(REXGLUE_PLATFORM_SWITCH)
    # The Switch-only forms of the first round (from sasrt-nx), each with its switch back:
    #   - no frame pointer in the game code: AArch64 GCC keeps the x29 frame chain in every non-leaf function by
    #     default, which costs the set-up instruction and one of the callee-saved registers the recompiled code is
    #     short of (27,354 game functions set one up in the build of 2026-10-12). The console profiler's pc samples
    #     are unaffected; its caller chains (switch_perf.cpp walks x29, bounds-checked) end at a game function.
    #   - flush-mode checks the control flow fixes compile to nothing (tools/flush_mode_flow.py);
    #   - guest compare-and-swap without the trailing `dmb ish` (tools/guest_cas.py; run time:
    #     banjo_cpu_guest_cas_no_barrier);
    #   - guest vector loads and stores reversed with REV64 + EXT (tools/vec_byteswap.py);
    #   - constant word shuffles as one permutation (tools/pch_neon_shuffles.py);
    #   - vperm and the float-to-integer conversions as their NEON instruction (rex/ppc/intrinsics.h).
    option(BANJO_SWITCH_OMIT_FRAME_POINTER "Game code without the x29 frame chain (Switch)" ON)
    option(BANJO_SWITCH_FLUSH_FLOW "Flush-mode checks proven by control flow compile to nothing (Switch)" ON)
    option(BANJO_SWITCH_GUEST_CAS "Guest compare-and-swap without the trailing dmb (Switch)" ON)
    option(BANJO_SWITCH_VEC_BYTESWAP "Guest vector byte reversal as REV64 + EXT (Switch)" ON)
    option(BANJO_SWITCH_NEON_SHUFFLES "Constant word shuffles as one permutation (Switch)" ON)
    option(BANJO_SWITCH_NEON_VMX_HELPERS "vperm and float-to-integer conversions as NEON instructions (Switch)" ON)
    if(NOT BANJO_SWITCH_FLUSH_FLOW)
        list(APPEND _banjo_round_definitions REX_FLUSH_FLOW_OFF)
    endif()
    if(NOT BANJO_SWITCH_GUEST_CAS)
        list(APPEND _banjo_round_definitions REX_GUEST_CAS_LEGACY)
    endif()
    if(NOT BANJO_SWITCH_VEC_BYTESWAP)
        list(APPEND _banjo_round_definitions REX_BSWAP128_TABLE)
    endif()
    if(NOT BANJO_SWITCH_NEON_SHUFFLES)
        list(APPEND _banjo_round_definitions REX_SIMDE_SHUFFLES)
    endif()
    if(NOT BANJO_SWITCH_NEON_VMX_HELPERS)
        list(APPEND _banjo_round_definitions REX_VMX_HELPERS_X86_FORM)
    endif()
    if(BANJO_SWITCH_OMIT_FRAME_POINTER AND TARGET renut_recomp)
        target_compile_options(renut_recomp PRIVATE -fomit-frame-pointer)
        # The localized copies are game code too (sa2-nx: the same rule for that file).
        set_property(SOURCE ${BANJO_HOT_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -fomit-frame-pointer)
    endif()
endif()
foreach(_banjo_target renut_recomp renut)
    if(TARGET ${_banjo_target} AND _banjo_round_definitions)
        target_compile_definitions(${_banjo_target} PRIVATE ${_banjo_round_definitions})
    endif()
endforeach()
message(STATUS "banjo: sa2-nx's CPU rounds: definitions '${_banjo_round_definitions}'")

# banjo-nx (2026-10-21, the user's request): link-time and profile-guided optimisation of the Switch build, from
# sa2-nx (its app/CMakeLists.txt and docs/enhancements.md, "Link-time optimisation of a recompiled program"). Both OFF
# in the everyday build folder (app/out/switch): a release is built in folders of its own (docs/pgo-lto.md).
#   - BANJO_SWITCH_LTO: the recompiled game (renut_recomp), the app (renut), Renut's renderer (banjo_renut_gpu) and
#     plume are compiled to GIMPLE (-flto -fno-fat-lto-objects) and optimised as one program at the link (-flto=N
#     -flto-partition=balanced: N parallel LTRANS jobs, low for the build machine's memory). The SDK libraries and uam
#     stay out. The link repeats -ffp-contract=off and -fno-strict-aliasing (the LTRANS stage must not fuse what the
#     compile stage kept apart), and xxHash is built with XXH_FORCE_MEMORY_ACCESS=0: nfsmw-nx's LTO+PGO builds hit
#     xxHash's default (a strict-aliasing violation) once it was inlined into a cache-key function (garbage keys).
#   - BANJO_PGO=generate: an instrumented NRO that streams its branch counts to pgo/ next to the NRO
#     (src/banjo_pgo.cpp; registered in a section, not by constructors: libgcov's exit-time files do not fit the
#     Switch). Value profiling is off: its shared lists are not safe without atomic updates.
#   - BANJO_PGO=use: the counts merged on the PC (tools/pgo_merge.py) from BANJO_PGO_DIR (pgo/ at the root): one
#     .gcda per object, named by the object's path in the build folder with # for each separator
#     (-fprofile-prefix-path), so the instrumented and the optimised builds may use different build folders. Code the
#     sessions did not run is compiled as without a profile (-fprofile-partial-training); functions changed since the
#     instrumented build keep no profile (-Wno-coverage-mismatch). GCC instruments before its interprocedural inliner,
#     so the instrumented build needs no LTO: only the sources and the compile options must match.
#   - With a profile, the game code's inliner limits (sa2-nx's measurements): max-inline-insns-auto 64 (the
#     compiler's 30 refused call sites that ran billions of times) and inline-unit-growth 12 (without it the higher
#     limit fills the default 40 per cent with code the sessions never ran). Compile options that do not touch a
#     recorded profile.
#   - sasrt-nx (2026-11-11, port 81; machine copies in place): the copies' two forms
#     (SASRT_HOT_IN_PLACE above) compile to different code, so each has a profile of its own: BANJO_PGO_DIR (pgo/)
#     for the hooked form, BANJO_PGO_DIR_IN_PLACE (pgo-inplace/, sasrt-nx's name) for the in-place one. A folder
#     writes (generate) and reads (use) the one of its form; a use build stops at the configure while that folder has
#     no .gcda file.
if(REXGLUE_PLATFORM_SWITCH)
    option(BANJO_SWITCH_LTO "Link-time optimisation of the game code, the app and Renut's renderer (Switch)" OFF)
    set(BANJO_SWITCH_LTO_JOBS "4" CACHE STRING "Parallel LTRANS jobs of the Switch LTO link")
    option(BANJO_SWITCH_IPA_PTA "-fipa-pta at the Switch LTO link (sa2-nx and UnleashedRecomp-NX use it)" OFF)
    set(BANJO_SWITCH_INLINE_AUTO "64" CACHE STRING
        "max-inline-insns-auto of the game code with a profile (empty: the compiler's)")
    set(BANJO_SWITCH_INLINE_UNIT_GROWTH "12" CACHE STRING
        "inline-unit-growth of the game code with a profile (empty: the compiler's)")
    set(BANJO_PGO "" CACHE STRING "Profile-guided optimisation of the Switch build: empty, generate or use")
    set_property(CACHE BANJO_PGO PROPERTY STRINGS "" generate use)
    set(BANJO_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
    # sasrt-nx (2026-11-11, port 81; machine copies in place): the in-place form's profile.
    set(BANJO_PGO_DIR_IN_PLACE "${CMAKE_CURRENT_SOURCE_DIR}/../pgo-inplace" CACHE PATH
        "Folder of the PGO profile (.gcda files) of the in-place copies' form (SASRT_HOT_IN_PLACE)")
    if(SASRT_HOT_IN_PLACE)
        set(_banjo_pgo_profile "${BANJO_PGO_DIR_IN_PLACE}")
    else()
        set(_banjo_pgo_profile "${BANJO_PGO_DIR}")
    endif()
    set(_banjo_opt_targets renut_recomp renut banjo_renut_gpu plume)
    # renut-ultimate (2026-11-07): the SDK runtime (kernel calls, threads, the audio driver) and FFmpeg (XMA decoding) too,
    # their C and C++ only: about 8 % of busy CPU in the console profile of 3 Nov. They were out only because every build
    # folder shared the SDK's libraries (REXGLUE_OUTPUT_IN_BUILD above).
    option(BANJO_SWITCH_SDK_OPT "The SDK runtime and FFmpeg in the Switch LTO and PGO (renut-ultimate)" ON)
    set(_banjo_sdk_opt_targets)
    if(BANJO_SWITCH_SDK_OPT AND REXGLUE_OUTPUT_IN_BUILD)
        set(_banjo_sdk_opt_targets rexruntime libavcodec libavutil)
    endif()
    # renut-ultimate (2026-11-12, port 89; sasrt-nx's batch B, B1, Blur's finding): the SDK's OBJECT libraries are compiled
    # with their own options, so they had neither LTO nor PGO. sasrt-nx's console race (in place, handheld clocks):
    # process CPU -0.7 %, render thread -1.9 %, its audio worker -4.9 %. Four of their files stay out of LTO
    # (_banjo_sdk_no_lto, below): threading_switch.cpp and switch_perf.cpp define --wrap's __wrap_ functions, which the
    # link must see as ordinary definitions; exception_handler_switch.cpp and fiber_switch.cpp have top-level asm that
    # names C symbols (g_rex_exc_*, rex_fiber_trampoline), invisible to LTO, which would drop them.
    option(BANJO_SWITCH_SDK_OBJECT_OPT "The SDK's OBJECT libraries in the Switch LTO and PGO too (port 89)" ON)
    set(_banjo_sdk_no_lto)
    if(BANJO_SWITCH_SDK_OPT AND REXGLUE_OUTPUT_IN_BUILD AND BANJO_SWITCH_SDK_OBJECT_OPT)
        list(APPEND _banjo_sdk_opt_targets rexaudio rexcore rexfilesystem rexinput rexui)
        set(_banjo_sdk_no_lto
            "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src/core/threading_switch.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src/ui/switch_perf.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src/core/exception_handler_switch.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src/core/fiber_switch.cpp")
    endif()
    if(BANJO_SWITCH_LTO)
        set(_banjo_lto_compile -flto -fno-fat-lto-objects)
        set(_banjo_lto_link -flto=${BANJO_SWITCH_LTO_JOBS} -flto-partition=balanced -ffp-contract=off
                            -fno-strict-aliasing)
        if(BANJO_SWITCH_IPA_PTA)
            list(APPEND _banjo_lto_compile -fipa-pta)
            list(APPEND _banjo_lto_link -fipa-pta)
        endif()
        foreach(target ${_banjo_opt_targets})
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_banjo_lto_compile})
                target_compile_definitions(${target} PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
            endif()
        endforeach()
        foreach(target ${_banjo_sdk_opt_targets})  # renut-ultimate (2026-11-07)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${_banjo_lto_compile}>")
                target_compile_definitions(${target} PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
            endif()
        endforeach()
        target_link_options(renut PRIVATE ${_banjo_lto_link})
        if(_banjo_sdk_no_lto)  # port 89: after the targets' -flto, so it wins
            set_property(SOURCE ${_banjo_sdk_no_lto} TARGET_DIRECTORY rexcore rexui APPEND PROPERTY COMPILE_OPTIONS
                         -fno-lto)
        endif()
        message(STATUS "banjo: Switch LTO (${BANJO_SWITCH_LTO_JOBS} jobs)")
    endif()
    get_filename_component(_banjo_pgo_dir "${_banjo_pgo_profile}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_banjo_pgo_dir}" _banjo_pgo_dir)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _banjo_pgo_prefix)
    if(BANJO_PGO STREQUAL "generate")
        set(_banjo_pgo "-fprofile-generate=${_banjo_pgo_dir}" "-fprofile-prefix-path=${_banjo_pgo_prefix}"
                       -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(renut PRIVATE BANJO_PGO_GENERATE=1)
    elseif(BANJO_PGO STREQUAL "use")
        # Port 81: a folder without .gcda files is no profile either (pgo-inplace/ before its first session).
        file(GLOB _banjo_pgo_gcda "${_banjo_pgo_profile}/*.gcda")
        if(NOT _banjo_pgo_gcda)
            if(SASRT_HOT_IN_PLACE)
                message(FATAL_ERROR "BANJO_PGO=use: no profile in ${_banjo_pgo_profile}, the in-place copies' form's "
                        "(SASRT_HOT_IN_PLACE ON). Make one with an instrumented build of this form (docs/building.md, "
                        "section 4), or configure with -DSASRT_HOT_IN_PLACE=OFF (IN_PLACE=OFF) for the hooked form "
                        "and its profile in ${BANJO_PGO_DIR}")
            else()
                message(FATAL_ERROR "BANJO_PGO=use: no profile in ${_banjo_pgo_profile}")
            endif()
        endif()
        # -fprofile-correction: the counts were taken without atomic updates (-fprofile-update=single) while several
        # threads ran the same code, so a few increments were lost and some blocks' counts do not add up; GCC
        # smooths them instead of stopping.
        set(_banjo_pgo "-fprofile-use=${_banjo_pgo_dir}" "-fprofile-prefix-path=${_banjo_pgo_prefix}"
                       -fprofile-correction -fno-profile-values -fprofile-partial-training -Wno-missing-profile
                       -Wno-coverage-mismatch)
        set(_banjo_inline_options "")
        if(BANJO_SWITCH_INLINE_AUTO)
            list(APPEND _banjo_inline_options --param=max-inline-insns-auto=${BANJO_SWITCH_INLINE_AUTO})
        endif()
        if(BANJO_SWITCH_INLINE_UNIT_GROWTH)
            list(APPEND _banjo_inline_options --param=inline-unit-growth=${BANJO_SWITCH_INLINE_UNIT_GROWTH})
        endif()
        if(_banjo_inline_options AND TARGET renut_recomp)
            target_compile_options(renut_recomp PRIVATE ${_banjo_inline_options})
            set_property(SOURCE ${BANJO_HOT_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${_banjo_inline_options})
        endif()
    elseif(BANJO_PGO)
        message(FATAL_ERROR "BANJO_PGO must be empty, generate or use (got ${BANJO_PGO})")
    endif()
    if(BANJO_PGO)
        foreach(target ${_banjo_opt_targets})
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_banjo_pgo})
            endif()
        endforeach()
        foreach(target ${_banjo_sdk_opt_targets})  # renut-ultimate (2026-11-07)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${_banjo_pgo}>")
            endif()
        endforeach()
        target_link_options(renut PRIVATE ${_banjo_pgo})
        message(STATUS "banjo: PGO ${BANJO_PGO} (${_banjo_pgo_profile})")
    endif()
endif()

# The app includes imgui.h and xxhash.h directly. An installed SDK package puts
# them in its single include folder; when the SDK is built from source
# (REXSDK_DIR) they are private to rexruntime, so add them here.
# SPIR-V initializers of the native renderer's EDRAM transfer / utility shaders (src/me/masseffect/shaders), compiled
# on the host with glslangValidator (tools/build_glslang.bat builds it from the SDK's glslang). For the Switch build
# the same host tool is used: SPIR-V is platform independent.
set(BANJO_GLSLANG_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/../out/glslang/StandAlone/glslangValidator.exe"
    CACHE FILEPATH "Host glslangValidator")
set(BANJO_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
set(BANJO_EDRAM_SHADER_INCLUDES)
foreach(_shader masseffect_edram_7e3_to_rgba8 masseffect_edram_rgba8_to_7e3 masseffect_edram_16f_to_16f me_resolve_exp_bias me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_import me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2 me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_depth_to_raw64 me_edram_raw64_to_depth me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2 me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_edram_color_to_color me_edram_r64_to_r64 sa2_resolve_copy)
    set(_extension comp)
    if(_shader STREQUAL "me_edram_import")
        set(_extension vert)
    elseif(_shader MATCHES "^me_edram_(color_to_depth|depth_to_depth|depth_to_stencil|color_to_stencil|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x|raw64_to_depth|color_to_color|r64_to_r64)$" OR _shader STREQUAL "sa2_resolve_copy")
        set(_extension frag)
    endif()
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/src/me/masseffect/shaders/${_shader}.${_extension}")
    set(_output "${BANJO_GENERATED_SHADER_DIR}/${_shader}.inc")
    set(_target_flags)
    if(_shader MATCHES "^me_edram_(stencil_to_buffer|color_stencil_to_buffer|stencil_msaa2_to_buffer|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x)$" OR _shader STREQUAL "me_depth_resolve_guestspace_msaa2")
        set(_target_flags --target-env vulkan1.2)
        set(_output "${BANJO_GENERATED_SHADER_DIR}/${_shader}_generated.inc")
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${BANJO_GENERATED_SHADER_DIR}"
        COMMAND "${BANJO_GLSLANG_VALIDATOR}" -V ${_target_flags} -x -o "${_output}" "${_source}"
        DEPENDS "${_source}" VERBATIM)
    list(APPEND BANJO_EDRAM_SHADER_INCLUDES "${_output}")
endforeach()
add_custom_target(banjo_edram_shaders DEPENDS ${BANJO_EDRAM_SHADER_INCLUDES})
add_dependencies(renut banjo_edram_shaders)
target_include_directories(renut PRIVATE
    "${BANJO_GENERATED_SHADER_DIR}"
    src/me
    src/me/masseffect
    src/me/masseffect/shaders
    "${REXSDK_DIR}/src/core")

# CPU-only SDK sources the native renderer reuses (draw-extent estimation, shader microcode interpretation): the
# GPU plugin's private symbols cannot satisfy application-side use.
target_sources(renut PRIVATE
    "${REXSDK_DIR}/src/graphics/register_file.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")

# Precompiled SPIR-V of the SDK presenter (vulkan_spirv/guest_output_*): the native renderer draws the game
# image into the presenter output with it.
target_include_directories(renut PRIVATE "${REXSDK_DIR}/src/ui/shaders")
# blur-nx (2026-11-18, the game menu's font option): Dear ImGui's misc/fonts as byte arrays (game_menu_fonts.h). Their
# licences: THIRD_PARTY_NOTICES.md.
set(_blur_menu_fonts_dir "${REXSDK_DIR}/thirdparty/imgui/misc/fonts")
set(_blur_menu_fonts_text "// Made by app/CMakeLists.txt from ${_blur_menu_fonts_dir} (do not edit).\n#pragma once\n")
foreach(_pair "kMenuFontProggyTiny:ProggyTiny.ttf" "kMenuFontRoboto:Roboto-Medium.ttf" "kMenuFontKarla:Karla-Regular.ttf"
              "kMenuFontCousine:Cousine-Regular.ttf")
    string(REPLACE ":" ";" _parts "${_pair}")
    list(GET _parts 0 _var)
    list(GET _parts 1 _file)
    file(READ "${_blur_menu_fonts_dir}/${_file}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
    string(APPEND _blur_menu_fonts_text "alignas(4) static const unsigned char ${_var}[] = {${_hex}};\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_blur_menu_fonts_dir}/${_file}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_menu_fonts/game_menu_fonts.h" "${_blur_menu_fonts_text}")
target_include_directories(renut PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/game_menu_fonts")

foreach(_dep imgui xxhash renderdoc)
    if(TARGET ${_dep})
        target_include_directories(renut PRIVATE
            $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# Nintendo Switch: renut.nro next to the ELF. On the SD card it goes in
# sdmc:/switch/banjo-nx/ with the extracted disc in game_root/ beside it.
if(REXGLUE_PLATFORM_SWITCH)
    # Link map: which object each function came from, for ranking the profiler's samples per function
    # (tools/switch-cpu-profile.py). It must come from the same build as the NRO that was profiled.
    target_link_options(renut PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/renut.map")
    # katamari-nx (2026-11-02, the user: only these settings in the toml, the rest hardcoded; sa2-nx's design): the fixed
    # settings compiled in (src/renut_app.h BuiltInConfig), and the player's katamari.toml, written next to the NRO when
    # it is missing (DefaultConfigFile). KATAMARI_BUILTIN_CONFIG=OFF: test builds that read a full renut.toml.
    option(KATAMARI_BUILTIN_CONFIG "Switch: the fixed settings compiled in, the player's options in katamari.toml" ON)
    if(KATAMARI_BUILTIN_CONFIG)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/katamari_switch_defaults.toml" KATAMARI_BUILTIN_TEXT)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/katamari_switch.toml" KATAMARI_PLAYER_TEXT)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/katamari_builtin_config.h.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/katamari_builtin_config.h" @ONLY)
        target_include_directories(renut PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
        target_compile_definitions(renut PRIVATE KATAMARI_BUILTIN_CONFIG=1)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/katamari_switch_defaults.toml"
                     "${CMAKE_CURRENT_SOURCE_DIR}/katamari_switch.toml")
    endif()
    set(RENUT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icon/katamari_icon.jpg")
    set(RENUT_ICON_ARG)
    if(EXISTS "${RENUT_ICON}")
        set(RENUT_ICON_ARG ICON "${RENUT_ICON}")
    endif()
    # katamari-nx (2026-11-02, the user's request): author ChanseyIsTheBest, the user's icon (the cover art).
    # 2026-11-05: version 1.0.1, the game's memory and the NRO's code on 2 MB blocks, and the boot's cvars.
    rexglue_switch_add_nro(renut
        NAME "Beautiful Katamari"
        AUTHOR "ChanseyIsTheBest"
        VERSION "1.0.1"
        ${RENUT_ICON_ARG})
endif()

# PC: line tables (CodeView) and a PDB, so the sampling profiler's addresses (nfsmw_perfil_pc) and crashes can be
# named with llvm-symbolizer. Debug info only: the generated code is the same.
if(WIN32)
    target_compile_options(renut PRIVATE -gline-tables-only -gcodeview)
    target_link_options(renut PRIVATE "LINKER:/DEBUG:FULL")
endif()

if(REXGLUE_PLATFORM_SWITCH AND BANJO_SWITCH_RENDERER_LINES)
    set_source_files_properties(src/me/me_native_system.cpp src/me/masseffect/masseffect_native_draws.cpp
        src/me/masseffect/masseffect_native_targets.cpp PROPERTIES COMPILE_OPTIONS "-g1")
endif()
