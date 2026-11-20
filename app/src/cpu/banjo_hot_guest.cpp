// banjo-nx - native replacements of hot guest functions (CPU enhancements banjo_cpu_native_*).
//
// The mechanism is sa2-nx's (app/src/cpu/sa2_hot_guest.cpp, GPL-3.0; it came there from masseffect-nx's
// me_hot_guest.cpp): each hook replaces one recompiled function by a native version and checks itself. Banjo's
// natives are all "localized copies" written by tools/localize_native.py (app/generated/hot/n_<id>.h, listed in
// app/src/cpu/hot/localized.txt): the recompiled function's own statements with its context registers in C++ locals.
//
// THE GUARD. For the first banjo_hot_guard_calls calls of each thread, one call in banjo_hot_guard_period afterwards
// and EVERY call with banjo_cpu_verify, a call is answered like this:
//   1. the copy's logged form runs on a copy of the registers: every guest store is recorded with the bytes it
//      replaces; the memory is rolled back from that log;
//   2. the ORIGINAL statements' logged form runs the same way and is rolled back;
//      (if either reached a call it cannot repeat - anything outside the copy: an indirect call, a kernel import, a
//      hooked function - the call is "not comparable": the recompiled original answers it in verify mode, the copy
//      otherwise, and it is counted;)
//   3. the bytes either of them wrote are the compare set: they are saved, the real copy runs on a copy of the
//      registers, its bytes are taken and the saved ones put back;
//   4. the recompiled original runs for real;
//   5. every register of the context, the written bytes and the host's flush mode are compared, and so are the
//      registers of the two logged runs.
// A difference that repeats when the whole pass is made again from the same inputs (three passes in a row) logs
// "[hot] DIFFERENCE ...", counts as a difference of the verify session (me::native::VerifyDifference) and switches
// that hook off for the rest of the run: the original had already produced the state. One that does not repeat came
// from another thread changing the inputs between the runs; it is counted apart ("did not repeat").
// A hook also stays off unless the loaded guest code is byte for byte the code its copy was made from.
//
// WHY EXACT: the copies are the same statements (tools/localize_native.py says what is renamed); the guard runs both.
// RISK: a difference on inputs the guard did not see; state another thread changes between the guard's runs is
//   reported as a difference (a false one) and only costs that hook.
// KEYS: banjo_cpu_native_<function> each (CPU group, under banjo_cpu_enhancements); banjo_hot_guard_calls,
//   banjo_hot_guard_period; banjo_hot_count (a measurement, see Counted).
// BUILD: sasrt-nx (2026-11-10, port 78; its idea 27): the guard runs the copies' two logged forms, which only a build
//   with CMake SASRT_HOT_GUARD_FORMS has (ON by default on the PC; OFF on the Switch, where tools/build_switch_guard.bat
//   makes an NRO with them). Without them every hook calls its copy directly and InitHook says so once.
//   sasrt-nx (2026-11-11, port 81; machine copies in place): with CMake SASRT_HOT_IN_PLACE (ON by
//   default on the Switch wherever the guard forms are OFF) there are no hooks at all: the generated function's
//   symbol is the copy (banjo_hot_guest.h), nothing of the guard runs and no copy's key is read; InPlaceNotes says so
//   once at start-up and names each key set against the build. The hand-written natives keep their own keys.
//
// Name no guest function's address in this file: tools/direct_calls.py and tools/localize_native.py treat every
// 82xxxxxx address the app's sources mention as hooked. The list is app/src/cpu/hot/localized.txt.

// The generated code's macros and helpers: the copies are made of generated statements.
#include "renut_pch.h"

// banjo-nx (2026-10-19): whether the generated code marks the store cards (tools/store_cards.py ran and
// REX_STORE_CARDS_OFF is not set): Renut proves its cached copies by them only then (banjo_renut_store_cards).
#if defined(REX_CARD) && !defined(REX_STORE_CARDS_OFF)
extern "C" const bool g_banjo_store_cards = true;
#else
extern "C" const bool g_banjo_store_cards = false;
#endif

#include "banjo_hot_guest.h"

#include <algorithm>
#include <atomic>
#include <cctype>  // blur-nx (2026-11-16): banjo_hot_guard_keep
#include <chrono>
#include <cmath>    // port 72: the VMX fuzz's absolute error
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/platform.h>
#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "cpu/banjo_enhancements.h"
#include "me/me_verify.h"

#include "cpu/hot/banjo_hand_natives.h"  // the hand-written natives the copies ask first (hot/hand.txt)
#include "hot/banjo_hot_all.h"  // app/generated/hot: the copies (tools/localize_native.py)

REXCVAR_DECLARE(bool, banjo_cpu_verify);
// sasrt-nx (2026-11-11, port 74; its idea 35): the copies with atomics (port 70) and every copy that reaches a call
// outside itself stop the guard's logged forms there: such a call was "not comparable". When both forms stopped, the
// bytes each wrote before its stop are compared too (the same inputs, the same path: the same stores up to the first
// atomic or outside call). Counted and logged only: a hand-written native in front of a callee can make the two forms
// stop at different points, and another thread can change a counter between the two runs (sasrt-nx's console: 6 of
// 5.07 million differed, one byte each), so a difference here does not switch the copy off.
// blur-nx (2026-11-13, #69 of docs/cpu-near-exact-50-1113.md's second wave): the guard's register compare takes a NaN
// that differs only in its sign bit (a float register, or a vector lane) as the same. The sign of a NaN an arithmetic
// operation makes is the compiler's choice (a negation folded into a multiply-add or not), so the copy and the generated
// code can differ there with the same source: the 4 differences of 2026-11-13's guard race (frsqrte of 0, then inf x 0).
// Counted apart in the [hot] summary. Memory is still compared byte for byte.
REXCVAR_DEFINE_BOOL(banjo_cpu_verify_nan_sign, true, "Banjo/Enhancements",
                    "The guard: a register NaN differing only in its sign bit counts as the same (counted apart); "
                    "false: every bit compared")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
std::atomic<uint64_t> g_guard_nan_sign_only{0};
REXCVAR_DEFINE_BOOL(banjo_hot_guard_prefix, true, "Banjo/Enhancements",
                    "The guard: when the copy and the original both stop (an atomic or a call outside the copy), compare "
                    "the stores each made before it (counted in the [hot] summary; a difference is logged, not acted on)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 72; its idea 43 F1-F4): tools/vmx_float_macros.py's fast forms against the exact ones, on
// the console itself (x86 cannot reproduce NEON's estimates). With the key, a thread started at the first hook runs N
// million lanes of random bit patterns (plus the special values) through each pair with the flush mode on (as the
// game's VMX code runs) and logs the largest difference in units in the last place and the special cases that differ
// (NaN, infinity, sign, zero). Only reports; nothing in the game changes. Switch builds with the fast forms only.
REXCVAR_DEFINE_INT32(sasrt_test_vmx_fuzz, 0, "Banjo/Enhancements",
                     "Test: compare the fast VMX estimate and multiply-add forms with the exact ones over N million "
                     "lanes on the console (\"[vmxfuzz]\" lines; 0 off)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-16, the user: "make it so you can turn off certain functions from the guard switch off"): the
// functions whose hook the guard never switches off. It still compares them and logs a repeated difference (the first
// few per hook), but the hook stays on. A list of guest addresses or names, comma separated ("8230AD50,822DA210" or
// "sub_8230AD50"); empty: the guard as before.
REXCVAR_DEFINE_STRING(banjo_hot_guard_keep, "", "Banjo/Enhancements",
                      "The guard: functions (comma-separated guest addresses or sub_ names) whose native copy it never "
                      "switches off; a difference is logged and the copy stays on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_hot_guard_calls, 64, "Banjo/Enhancements",
                     "Native copies of hot game functions: the first N guarded calls of each thread come one after "
                     "the other (each runs the copy and the recompiled original and compares them)")
    .range(0, 4000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_hot_guard_period, 4096, "Banjo/Enhancements",
                     "Native copies of hot game functions: after the first N, one call in P is guarded (rounded up to a "
                     "power of two, at most 1048576). 0 with banjo_hot_guard_calls = 0: no guard at all, every hook "
                     "calls its copy directly (no countdown; bejeweled3-nx's direct dispatch)")
    .range(0, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-22, bejeweled3-nx's method): the bulk list's copies (app/src/cpu/hot/localized_bulk.txt, hooks
// in app/src/cpu/banjo_hot_shard_<k>.cpp) under one master key; each also has its own (category Banjo/Bulk copies).
REXCVAR_DEFINE_BOOL(banjo_cpu_native_bulk, true, "Banjo/Enhancements",
                    "Bulk localized copies: every game function the console profiles found hot that still ran as "
                    "generated code, as a localized copy (registers in C++ locals; tools/localize_native.py --bulk). "
                    "false = all of them back to the generated code (their own keys: banjo_cpu_native_sub_*)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-12; port 121): see BANJO_HOT_POISON (banjo_hot_guest.h).
REXCVAR_DEFINE_BOOL(banjo_hot_poison_dead, false, "Banjo/Enhancements",
                    "Check (hooked builds): after each unguarded run of a copy, overwrite the registers the guard does "
                    "not compare (dead after the return by LiveOut: tools/reg_liveness.py, leaf_dead_stores.py) with "
                    "garbage, so a wrong LiveOut shows. Diagnostic only; costs a test per call")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_hot_bulk_lines, false, "Banjo/Diagnostics",
                    "Verify runs (2026-11-19): about once a minute the guard's report also has a line per reached bulk "
                    "copy ('[hot] bulk reached <name> on|off: N compared, M not comparable'), to see which copies of a "
                    "new list a run reached")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_hot_count, false, "Banjo/Enhancements",
                    "Measurement (PC): every native copy runs in its counting form and logs/hot_counts.txt gets how "
                    "often each block of statements ran, for tools/hot_arm_compare.py. No guard while it is on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The hand-written natives' own keys (app/src/cpu/hot/banjo_hand_natives.h; CPU group). Each stands in front of the
// generated copy of its function in every hook that contains it, so its key switches it off in all of them at once
// (the generated copies then run, as before).
REXCVAR_DEFINE_BOOL(banjo_hand_memcpy, true, "Banjo/Enhancements",
                    "Hand-written native of the game's C runtime memcpy (sa2-nx's): overlapping 16/8/4/2-byte moves "
                    "up to 128 bytes, the host's memcpy above; in front of the generated copy in every hook that "
                    "contains it. Declines overlapping forward smears; checked by those hooks' guards")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_hand_memset, true, "Banjo/Enhancements",
                    "Hand-written native of the game's C runtime memset (sa2-nx's): overlapping stores up to 64 "
                    "bytes, the host's memset above; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_hand_constants, true, "Banjo/Enhancements",
                    "Hand-written native of Direct3D's two shader float-constant writers (sa2-nx's): a plain copy of "
                    "the vectors and the dirty-mask update instead of the emulated unaligned vector loads; checked "
                    "by the guards of the hooks that contain them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(banjo_hand_state_flush, true, "Banjo/Enhancements",
                    "Hand-written native of the two writers of Direct3D's state flush (dirty register words and "
                    "dirty float-constant blocks into PM4 packets in the command buffer): the runs are copied in "
                    "guest byte order instead of word by word through two byte swaps. Declines a run that needs "
                    "more buffer space; checked by the guards of the hooks that contain them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(banjo_hand_audio_filter, true, "Banjo/Enhancements",
                    "Hand-written native of XAudio's per-voice state-variable filter (sub_82B6B258, the audio "
                    "thread's hottest guest function): the filter state in registers and four channels at a time "
                    "(NEON double lanes) instead of a byte-swapped load and store of every value at every sample; "
                    "bit-identical (the same double multiply-adds rounded to float), checked by the guard of its hook")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(banjo_hand_xaudio_mix, true, "Banjo/Enhancements",
                    "Hand-written native of XAudio's ramped channel mix (XAudio's MixFrameBufferChannel, the loop of "
                    "the audio thread's router: a voice's 256 samples times a gain ramp added to a submix channel): "
                    "the same vector multiplies and adds, sixteen samples a step with no store-card mark per vector "
                    "(one for the range); checked by the guard of the router's hook")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// katamari-nx (2026-10-30): Beautiful Katamari's own hand-written natives (cpu/hot/banjo_hand_natives.h, the end).
REXCVAR_DEFINE_BOOL(katamari_hand_matrix_copy, true, "Katamari/Enhancements",
                    "Hand-written native of the game's 4x4 matrix copy (sub_82759C98, the hottest guest function of "
                    "the PC profile): the 64 bytes moved as they are instead of sixteen float loads and stores. "
                    "Declines a signalling NaN and a forward overlap; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_matrix_multiply, true, "Katamari/Enhancements",
                    "Hand-written native of the game's two 4x4 matrix multiplies (sub_827598C0 in place, sub_82759780 "
                    "with three operands): the same sixteen dot products on registers, each row loaded and stored once "
                    "instead of the emulated unaligned vector loads and byte-by-byte stores; every register the "
                    "original writes is written. Checked by the guards of the hooks that contain them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_matrix_translation, true, "Katamari/Enhancements",
                    "Hand-written native of the game's translation-matrix builder (sub_82759D20): the 16 words "
                    "written at once. Declines a signalling NaN and an overlap; checked by the guards of its hooks")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(katamari_hand_matrix_identity, true, "Katamari/Enhancements",
                    "Hand-written native of the game's identity-matrix builder (sub_827595D0): the 16 words written "
                    "at once; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_matrix_compare, true, "Katamari/Enhancements",
                    "Hand-written native of the game's matrix comparison within a tolerance (sub_82759A00): the same "
                    "single-precision differences and compares in registers, the stack temporary written once; "
                    "checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_copy_words, true, "Katamari/Enhancements",
                    "Hand-written native of the C runtime's word copy (sub_827E73A8): overlapping 16/8/4/2-byte moves "
                    "instead of byte and word loops; declines a forward overlap; checked by the guards of its hooks")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_string_hash, true, "Katamari/Enhancements",
                    "Hand-written native of the game's string hash (sub_8266AF60, Jenkins' one-at-a-time): the bytes "
                    "walked through a host pointer with the same 64-bit adds and 32-bit shifts; checked by the guards "
                    "of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_vec4_update, true, "Katamari/Enhancements",
                    "Hand-written native of the game's 4-float parameter set with change detection (sub_8271FAD0): "
                    "the four compares and the writes in registers; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(katamari_hand_matrix_product, true, "Katamari/Enhancements",
                    "Hand-written native of the game's 4x4 matrix product r3 = r4 x r5 (sub_827596B0): the original's sixteen dot products on locals, one write of the result; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_vector_set, true, "Katamari/Enhancements",
                    "Hand-written native of the game's four-vector and scale set (sub_8270FE40): sixteen words, the scale and the flag in three writes; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_sphere_box, true, "Katamari/Enhancements",
                    "Hand-written native of the job threads' sphere test against a box (sub_82316A78): no flush-mode switch in the middle (scalar steps mode-free, vector steps checked for denormals), one write of the stack temporary; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_rotate_vectors, true, "Katamari/Enhancements",
                    "Hand-written native of the game's 3x3 rotation of N vectors (sub_8275A828): the matrix read once instead of for every vector; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_transform_points, true, "Katamari/Enhancements",
                    "Hand-written native of the game's 4x4 point transform with the divide by w (sub_8275AA88): the separate-output path, one formula per point; the in-place path stays generated; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_integrate, true, "Katamari/Enhancements",
                    "Hand-written native of the game's integration step of N bodies (sub_8276E3D8): the array's address read once instead of after every store; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_sine, true, "Katamari/Enhancements",
                    "Hand-written native of the game's sine (sub_824E97F0): the range reduction and the polynomial without the context and stack traffic, the stack temporaries written once; checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// katamari-nx (2026-11-01, rc10, the user: "optimise these two sub_82277E70 and sub_822683D0 by hand").
REXCVAR_DEFINE_BOOL(katamari_hand_transform_chain, true, "Katamari/Enhancements",
                    "Hand-written native of the objects' world transform (sub_82277E70): after its two real calls, "
                    "the matrix chain (copies, products, rotations, two translate calls) in registers instead of "
                    "through guest memory, every byte and register the original leaves; no guard can repeat it, so it "
                    "checks itself against the matrix natives in the original order (see the _verify key)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_transform_chain_verify, false, "Katamari/Enhancements",
                    "Check every call of katamari_hand_transform_chain against the matrix natives in the original's "
                    "order (slow). Off: the first 2000 calls of the run, then one in 256. A difference logs "
                    "\"[hot] DIFFERENCE sub_82277E70\" and switches the register path off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_box_test, true, "Katamari/Enhancements",
                    "Hand-written native of the visibility manager's box test (sub_82265C08, the hot loop of "
                    "sub_822683D0): the eight corners without the store / 4-byte store / reload of each through the "
                    "stack, the loop's invariant merges once, and the next object of the walk prefetched; checked by "
                    "the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(katamari_hand_translate_multiply, true, "Katamari/Enhancements",
                    "Hand-written native of the game's translate-then-multiply (sub_8223A970): the matrix copy, the "
                    "translation matrix and the product in registers, the frame's bytes as the original leaves them; "
                    "checked by the guards of the hooks that contain it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::hand {

uint8_t g_state[kNatives] = {};

namespace {
constexpr const char* kNames[kNatives] = {"memcpy", "memset", "constant writers", "state flush writers",
                                          "XAudio voice filter", "XAudio channel mix", "matrix copy",
                                          "matrix multiplies", "translation matrix", "identity matrix",
                                          "matrix comparison", "word copy", "string hash",
                                          "4-float parameter set", "matrix product", "four-vector set", "sphere-in-box test", "vector rotation", "point transform", "body integration", "sine", "transform chain", "box test", "translate and multiply"};
constexpr const char* kKeys[kNatives] = {"banjo_hand_memcpy", "banjo_hand_memset", "banjo_hand_constants",
                                         "banjo_hand_state_flush", "banjo_hand_audio_filter",
                                         "banjo_hand_xaudio_mix", "katamari_hand_matrix_copy",
                                         "katamari_hand_matrix_multiply", "katamari_hand_matrix_translation",
                                         "katamari_hand_matrix_identity", "katamari_hand_matrix_compare",
                                         "katamari_hand_copy_words", "katamari_hand_string_hash",
                                         "katamari_hand_vec4_update", "katamari_hand_matrix_product", "katamari_hand_vector_set", "katamari_hand_sphere_box", "katamari_hand_rotate_vectors", "katamari_hand_transform_points", "katamari_hand_integrate", "katamari_hand_sine", "katamari_hand_transform_chain", "katamari_hand_box_test", "katamari_hand_translate_multiply"};
bool Own(uint32_t which) {
  return which == kMemcpy           ? bool(REXCVAR_GET(banjo_hand_memcpy))
         : which == kMemset         ? bool(REXCVAR_GET(banjo_hand_memset))
         : which == kConstants      ? bool(REXCVAR_GET(banjo_hand_constants))
         : which == kFlush          ? bool(REXCVAR_GET(banjo_hand_state_flush))
         : which == kAudioFilter    ? bool(REXCVAR_GET(banjo_hand_audio_filter))
         : which == kXAudioMix      ? bool(REXCVAR_GET(banjo_hand_xaudio_mix))
         : which == kMatCopy        ? bool(REXCVAR_GET(katamari_hand_matrix_copy))
         : which == kMatMul         ? bool(REXCVAR_GET(katamari_hand_matrix_multiply))
         : which == kMatTranslation ? bool(REXCVAR_GET(katamari_hand_matrix_translation))
         : which == kMatIdentity    ? bool(REXCVAR_GET(katamari_hand_matrix_identity))
         : which == kMatCompare     ? bool(REXCVAR_GET(katamari_hand_matrix_compare))
         : which == kCopyWords      ? bool(REXCVAR_GET(katamari_hand_copy_words))
         : which == kStringHash     ? bool(REXCVAR_GET(katamari_hand_string_hash))
         : which == kMatProduct     ? bool(REXCVAR_GET(katamari_hand_matrix_product))
         : which == kVecSet         ? bool(REXCVAR_GET(katamari_hand_vector_set))
         : which == kSphereBox      ? bool(REXCVAR_GET(katamari_hand_sphere_box))
         : which == kRotate         ? bool(REXCVAR_GET(katamari_hand_rotate_vectors))
         : which == kTransform      ? bool(REXCVAR_GET(katamari_hand_transform_points))
         : which == kIntegrate      ? bool(REXCVAR_GET(katamari_hand_integrate))
         : which == kSine           ? bool(REXCVAR_GET(katamari_hand_sine))
         : which == kXformChain     ? bool(REXCVAR_GET(katamari_hand_transform_chain))
         : which == kBoxTest        ? bool(REXCVAR_GET(katamari_hand_box_test))
         : which == kXlateMul       ? bool(REXCVAR_GET(katamari_hand_translate_multiply))
                                    : bool(REXCVAR_GET(katamari_hand_vec4_update));
}
// banjo_hot_count: calls by size, bucket 0 = size 0, bucket k = sizes [2^(k-1), 2^k).
std::atomic<uint64_t> g_sizes[kNatives][34];
}  // namespace

// May run on several threads at once for the first call: every one of them stores the same value.
uint8_t Init(uint32_t which) {
  const uint8_t state = banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, Own(which)) ? 2 : 1;
  if (g_state[which] == 0)
    REXLOG_INFO("[hot] hand-written native of the {} ({}): {}", kNames[which], kKeys[which], state == 2 ? "ON" : "off");
  g_state[which] = state;
  return state;
}

void Count(uint32_t which, uint32_t n) {
  g_sizes[which][n ? 32 - __builtin_clz(n) : 0].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace banjo::hand

namespace banjo::hot {

namespace {

Hook* g_hooks = nullptr;  // every hook (static initialisation, one thread)

// The buffers of one guarded call. A guarded call can contain others (the recompiled original calls hooked
// functions, itself among them), so a thread has one of these per nesting depth. Reached only from Check: the
// thread_local costs the guarded calls alone.
struct Scratch {
  WriteLog log;
  std::vector<Region> ranges;
  std::vector<uint8_t> before;
  std::vector<uint8_t> native_out;
  PPCContext in;  // the registers the call came with
  size_t bytes = 0;
};
struct ThreadScratch {
  std::vector<Scratch*> frames;  // live as long as the thread (guest threads are few and long-lived)
  uint32_t depth = 0;
};
ThreadScratch& Thread() {
  static thread_local ThreadScratch* scratch = nullptr;
  if (!scratch) scratch = new ThreadScratch;
  return *scratch;
}
struct ScratchFrame {
  ThreadScratch& thread;
  Scratch& scratch;
  explicit ScratchFrame(ThreadScratch& t) : thread(t), scratch(Take(t)) {}
  ~ScratchFrame() {
    --thread.depth;
    // A guarded call that wrote megabytes (a memset of a loader's buffer among a thread's first calls) leaves its
    // log and its two copies of those bytes behind, about four times what it wrote, for as long as the thread
    // lives. On the console that memory is the same that textures and the driver need: large ones are given back.
    constexpr size_t kKeepBytes = 256u << 10;
    if (size_t(scratch.log.capacity) * sizeof(WriteLog::Entry) > kKeepBytes) {
      std::free(scratch.log.entries);
      scratch.log.entries = nullptr;
      scratch.log.capacity = scratch.log.count = 0;
    }
    if (scratch.ranges.capacity() * sizeof(Region) > kKeepBytes) std::vector<Region>().swap(scratch.ranges);
    if (scratch.before.capacity() > kKeepBytes) std::vector<uint8_t>().swap(scratch.before);
    if (scratch.native_out.capacity() > kKeepBytes) std::vector<uint8_t>().swap(scratch.native_out);
  }
  static Scratch& Take(ThreadScratch& t) {
    if (t.depth == t.frames.size()) t.frames.push_back(new Scratch);
    return *t.frames[t.depth++];
  }
};

bool GuestCodeMatches(const Hook& h, const uint8_t* base) {
  uint64_t fnv = 0xCBF29CE484222325ull;
  for (uint32_t r = 0; r < h.code_ranges; ++r) {
    const uint8_t* p = base + h.code[r].address;
    for (uint32_t i = 0; i < h.code[r].bytes; ++i) {
      fnv ^= p[i];
      fnv *= 0x100000001B3ull;
    }
  }
  if (fnv == h.code_hash) return true;
  REXLOG_WARN("[hot] {} ({}): the game code at {:08X} is not the code the copy was made from (hash {:016X}, expected "
              "{:016X}); it stays off",
              h.name, h.key, h.code[0].address, fnv, h.code_hash);
  return false;
}

const PPCRegister* Gpr(const PPCContext& c, int n) {
  switch (n) {
#define G(i) \
  case i:    \
    return &c.r##i;
    G(0) G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15) G(16) G(17) G(18) G(19)
    G(20) G(21) G(22) G(23) G(24) G(25) G(26) G(27) G(28) G(29) G(30) G(31)
#undef G
  }
  return nullptr;
}
const PPCRegister* Fpr(const PPCContext& c, int n) { return &c.f0 + n; }    // f0..f31 are consecutive
const PPCVRegister* Vr(const PPCContext& c, int n) { return &c.v0 + n; }    // v0..v127 are consecutive
const PPCCRRegister* Cr(const PPCContext& c, int n) { return &c.cr0 + n; }  // cr0..cr7 are consecutive

// First difference between two contexts (every register but the hook's skip set), or an empty string. a: the copy;
// b: the original.
// blur-nx (2026-11-13, #69, banjo_cpu_verify_nan_sign): x and y are the same NaN but for the sign bit.
bool NanSignOnly64(uint64_t x, uint64_t y) {
  return (x ^ y) == 0x8000000000000000ull && (x & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
         (x & 0x000FFFFFFFFFFFFFull) != 0;
}
bool NanSignOnly128(const PPCVRegister& x, const PPCVRegister& y) {
  for (int k = 0; k < 4; ++k) {
    const uint32_t a = x.u32[k], b = y.u32[k];
    if (a != b && !((a ^ b) == 0x80000000u && (a & 0x7F800000u) == 0x7F800000u && (a & 0x007FFFFFu) != 0))
      return false;
  }
  return true;
}

std::string CompareRegisters(const PPCContext& a, const PPCContext& b, const RegSkip& skip) {
  static const bool nan_sign = REXCVAR_GET(banjo_cpu_verify_nan_sign);
  for (int i = 0; i < 32; ++i) {
    if ((skip.gpr >> i) & 1) continue;
    if (Gpr(a, i)->u64 != Gpr(b, i)->u64)
      return fmt::format("r{} copy={:#018x} original={:#018x}", i, Gpr(a, i)->u64, Gpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    if ((skip.fpr >> i) & 1) continue;
    if (Fpr(a, i)->u64 != Fpr(b, i)->u64) {
      if (nan_sign && NanSignOnly64(Fpr(a, i)->u64, Fpr(b, i)->u64)) {
        g_guard_nan_sign_only.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      return fmt::format("f{} copy={:#018x} original={:#018x}", i, Fpr(a, i)->u64, Fpr(b, i)->u64);
    }
  }
  for (int i = 0; i < 128; ++i) {
    if ((skip.vr[i >> 6] >> (i & 63)) & 1) continue;
    if (std::memcmp(Vr(a, i)->u8, Vr(b, i)->u8, 16) != 0) {
      if (nan_sign && NanSignOnly128(*Vr(a, i), *Vr(b, i))) {
        g_guard_nan_sign_only.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const auto* x = Vr(a, i)->u32;
      const auto* y = Vr(b, i)->u32;
      return fmt::format("v{} copy={:08x}{:08x}{:08x}{:08x} original={:08x}{:08x}{:08x}{:08x}", i, x[0], x[1], x[2],
                         x[3], y[0], y[1], y[2], y[3]);
    }
  }
  for (int i = 0; i < 8; ++i) {
    if ((skip.cr >> i) & 1) continue;
    if (Cr(a, i)->raw() != Cr(b, i)->raw())
      return fmt::format("cr{} copy={:x} original={:x}", i, Cr(a, i)->raw(), Cr(b, i)->raw());
  }
  if (!skip.ctr && a.ctr.u64 != b.ctr.u64) return fmt::format("ctr copy={:#x} original={:#x}", a.ctr.u64, b.ctr.u64);
  if (!skip.xer && (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca)) return "xer";
  if (a.reserved.u64 != b.reserved.u64) return "reserved";
  if (a.lr != b.lr) return fmt::format("lr copy={:#x} original={:#x}", a.lr, b.lr);
  if (a.vscr_sat != b.vscr_sat) return "vscr_sat";
  // Only the guest-visible bits (rounding, flush): the host's sticky exception flags may differ harmlessly.
  constexpr uint32_t kGuest = rex::ppc::FPSCRRegister::GuestMask;
  if ((a.fpscr.csr & kGuest) != (b.fpscr.csr & kGuest))
    return fmt::format("fpscr.csr copy={:#x} original={:#x}", a.fpscr.csr, b.fpscr.csr);
  return {};
}

void AddRanges(const WriteLog& log, std::vector<Region>& ranges) {
  for (uint32_t i = 0; i < log.count; ++i) ranges.push_back({log.entries[i].addr, log.entries[i].len});
}

// Sorted, without overlaps.
void Merge(std::vector<Region>& ranges) {
  std::sort(ranges.begin(), ranges.end(), [](const Region& a, const Region& b) { return a.addr < b.addr; });
  size_t n = 0;
  for (const Region& r : ranges) {
    if (n && uint64_t(ranges[n - 1].addr) + ranges[n - 1].len >= r.addr) {
      const uint64_t end = std::max(uint64_t(ranges[n - 1].addr) + ranges[n - 1].len, uint64_t(r.addr) + r.len);
      ranges[n - 1].len = uint32_t(end - ranges[n - 1].addr);
    } else {
      ranges[n++] = r;
    }
  }
  ranges.resize(n);
}

// Port 74 (its idea 35): what one logged form wrote (its ranges merged and sorted, their bytes after it; `at` the
// ranges' offsets).
struct Written {
  std::vector<Region> ranges;
  std::vector<size_t> at;
  std::vector<uint8_t> bytes;
};

void CaptureWritten(const WriteLog& log, uint8_t* base, Written& w) {
  w.ranges.clear();
  w.at.clear();
  w.bytes.clear();
  AddRanges(log, w.ranges);
  Merge(w.ranges);
  for (const Region& r : w.ranges) {
    w.at.push_back(w.bytes.size());
    const uint8_t* p = Raw(base, r.addr);
    w.bytes.insert(w.bytes.end(), p, p + r.len);
  }
}

// The kept buffers back to small after a big call (sasrt-nx's batch 2 audit, finding 8, as ScratchFrame's rule).
void TrimWritten(Written& w) {
  if (w.bytes.capacity() > (256u << 10)) {
    w.bytes = {};
    w.ranges = {};
    w.at = {};
  }
}

// The byte at guest address x after the form that wrote `w` (memory as it is now where the form did not write: both
// forms are rolled back when this runs, so that is the state before either). A binary search (finding 7).
uint8_t AfterForm(const Written& w, uint8_t* base, uint32_t x) {
  auto it = std::upper_bound(w.ranges.begin(), w.ranges.end(), x,
                             [](uint32_t v, const Region& r) { return v < r.addr; });
  if (it != w.ranges.begin()) {
    --it;
    if (uint64_t(x) < uint64_t(it->addr) + it->len) return w.bytes[w.at[size_t(it - w.ranges.begin())] + (x - it->addr)];
  }
  return *Raw(base, x);
}

bool PrefixEqual(uint8_t* base, const Written& a, const Written& b, std::string& where) {
  std::vector<Region> all = a.ranges;
  all.insert(all.end(), b.ranges.begin(), b.ranges.end());
  Merge(all);
  for (const Region& r : all) {
    for (uint32_t k = 0; k < r.len; ++k) {
      const uint32_t x = r.addr + k;
      const uint8_t va = AfterForm(a, base, x), vb = AfterForm(b, base, x);
      if (va != vb) {
        where = fmt::format("memory {:#010x} copy={:#04x} original={:#04x}", x, va, vb);
        return false;
      }
    }
  }
  return true;
}

std::atomic<int64_t> g_last_report_ms{0};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void WriteCounts() {
  FILE* f = std::fopen("logs/hot_counts.txt", "w");
  if (!f) return;
  std::fprintf(f, "# banjo_hot_count: <function> <block> <entries>   (tools/hot_arm_compare.py)\n");
  for (Hook* h = g_hooks; h; h = h->next) {
    for (uint32_t i = 0; i < h->hit_count; ++i) {
      const uint64_t n = h->hits[i].load(std::memory_order_relaxed);
      if (n) std::fprintf(f, "%s %u %llu\n", h->name, i, static_cast<unsigned long long>(n));
    }
  }
  std::fclose(f);
}

}  // namespace

Hook::Hook(const char* name_, const char* key_, PPCFunc* orig_, void (*native_)(PPCContext&, uint8_t*),
           void (*native_logged_)(PPCContext&, uint8_t*, Logged&),
           void (*reference_logged_)(PPCContext&, uint8_t*, Logged&), void (*native_counted_)(PPCContext&, uint8_t*),
           std::atomic<uint64_t>* hits_, uint32_t hit_count_, const CodeRange* code_, uint32_t code_ranges_,
           uint64_t code_hash_, bool (*own_)(), uint32_t slot_, const RegSkip* skip_, bool bulk_)
    : name(name_),
      key(key_),
      orig(orig_),
      native(native_),
      native_logged(native_logged_),
      reference_logged(reference_logged_),
      native_counted(native_counted_),
      hits(hits_),
      hit_count(hit_count_),
      code(code_),
      code_ranges(code_ranges_),
      code_hash(code_hash_),
      own(own_),
      slot(slot_),
      skip(skip_),
      bulk(bulk_) {
  // In list order: the hooks are defined in the order of banjo_hot_list.inc, so append.
  Hook** tail = &g_hooks;
  while (*tail) tail = &(*tail)->next;
  *tail = this;
}

// sasrt-nx (2026-11-11, port 81; machine copies in place): the in-place copies' records
// (banjo_hot_guest.h), in list order within a file. Both pointers are constant-initialised, so a record of any file
// may append before this file's own dynamic initialisation has run.
namespace {
InPlaceCopy* g_in_place_list = nullptr;
InPlaceCopy** g_in_place_tail = &g_in_place_list;
}  // namespace

InPlaceCopy::InPlaceCopy(const char* name_, const char* key_, bool on_, bool bulk_, bool (*own_)())
    : name(name_), key(key_), on(on_), bulk(bulk_), own(own_) {
  *g_in_place_tail = this;
  g_in_place_tail = &next;
}

bool WriteLog::Grow() {
  if (capacity >= kMaxEntries) return false;
  const uint32_t wanted = capacity ? capacity * 2 : 4096;
  auto* grown = static_cast<Entry*>(std::realloc(entries, size_t(wanted) * sizeof(Entry)));
  if (!grown) return false;
  entries = grown;
  capacity = wanted;
  return true;
}

namespace {
// sasrt-nx (2026-11-11, port 72): a background thread started from a guest thread (InitHook runs on one). On the Switch a libnx
// thread: a std::thread created from the SDK's Horizon threads threw std::system_error at detach() and terminated the
// game at its start (run gv3, 2026-11-11 02:22; Ryujinx's stack: InitHook -> call_once -> std::thread::detach ->
// __cxa_throw -> terminate), as masseffect_deferred_recording.cpp found for a pthread. Priority 0x3B (an application's
// lowest: 0x3F was refused on the console, gv3's second run), any core.
void StartBackground(void (*fn)(void*), void* arg) {
#if REX_PLATFORM_SWITCH
  static ::Thread threads[4];  // (qualified: the namespace has a Thread of its own)
  static std::atomic<int> used{0};
  const int k = used.fetch_add(1, std::memory_order_relaxed);
  if (k >= 4 || !R_SUCCEEDED(::threadCreate(&threads[k], fn, arg, nullptr, 256 * 1024, 0x3B, -2)) ||
      !R_SUCCEEDED(::threadStart(&threads[k])))
    REXLOG_WARN("[hot] a background thread could not be started ({} of 4)", k + 1);
#else
  std::thread([fn, arg] { fn(arg); }).detach();
#endif
}

}  // namespace

#if defined(REX_VMX_FLOAT_HELPERS)
namespace {
// Port 72: sasrt-nx's fuzz test of its idea 43 (sasrt_test_vmx_fuzz above).
struct FuzzStat {
  uint64_t lanes = 0, differ = 0, special = 0;
  uint32_t max_ulp = 0;
  uint32_t worst_in[3] = {0, 0, 0};
  double max_abs = 0.0;  // sasrt-nx (2026-11-11): vlogefp's measure (its 360 bound is absolute: 1/32)
  uint32_t worst_abs_in = 0;
};
uint32_t UlpApart(uint32_t a, uint32_t b) {
  auto key = [](uint32_t v) { return (v & 0x80000000u) ? 0x80000000u - (v & 0x7FFFFFFFu) : 0x80000000u + v; };
  const uint32_t x = key(a), y = key(b);
  return x > y ? x - y : y - x;
}
void FuzzNote(FuzzStat& st, uint32_t exact, uint32_t fast, uint32_t a, uint32_t b, uint32_t c) {
  ++st.lanes;
  if (exact == fast) return;
  ++st.differ;
  const bool nan_e = (exact & 0x7FFFFFFFu) > 0x7F800000u, nan_f = (fast & 0x7FFFFFFFu) > 0x7F800000u;
  const bool inf_e = (exact & 0x7FFFFFFFu) == 0x7F800000u, inf_f = (fast & 0x7FFFFFFFu) == 0x7F800000u;
  if (nan_e != nan_f || inf_e != inf_f || ((exact ^ fast) & 0x80000000u)) {
    ++st.special;
    return;
  }
  if (nan_e) return;  // both NaN (payloads aside)
  const uint32_t u = UlpApart(exact, fast);
  if (u > st.max_ulp) {
    st.max_ulp = u;
    st.worst_in[0] = a, st.worst_in[1] = b, st.worst_in[2] = c;
  }
  float fe, ff;
  std::memcpy(&fe, &exact, 4);
  std::memcpy(&ff, &fast, 4);
  if (const double d = std::fabs(double(fe) - double(ff)); d > st.max_abs) {
    st.max_abs = d;
    st.worst_abs_in = a;
  }
}
void VmxFuzz(int millions) {
  uint64_t fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  __asm__ __volatile__("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)) : "memory");  // FZ, as the game's VMX code runs
  uint64_t s = 0x9E3779B97F4A7C15ull;
  auto next = [&s] {
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return uint32_t(s);
  };
  static const uint32_t kSpecial[] = {0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u,
                                      0x00000001u, 0x807FFFFFu, 0x00800000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu};
  FuzzStat rs, re, ma, nm, lg;
  const uint64_t n = uint64_t(millions) * 1000000ull / 4 + 3;
  for (uint64_t i = 0; i < n; ++i) {
    alignas(16) uint32_t a[4], b[4], c[4], e[4], f[4];
    for (int k = 0; k < 4; ++k) {
      a[k] = i < 3 ? kSpecial[(i * 4 + k) % 12] : next();
      b[k] = next();
      c[k] = next();
    }
    const simde__m128 va = simde_mm_load_ps(reinterpret_cast<const float*>(a));
    const simde__m128 vb = simde_mm_load_ps(reinterpret_cast<const float*>(b));
    const simde__m128 vc = simde_mm_load_ps(reinterpret_cast<const float*>(c));
    simde_mm_store_ps(reinterpret_cast<float*>(e), simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(va)));
    simde_mm_store_ps(reinterpret_cast<float*>(f), rex_vrsqrtefp_fast_(va));
    for (int k = 0; k < 4; ++k) FuzzNote(rs, e[k], f[k], a[k], 0, 0);
    simde_mm_store_ps(reinterpret_cast<float*>(e), simde_mm_div_ps(simde_mm_set1_ps(1), va));
    simde_mm_store_ps(reinterpret_cast<float*>(f), rex_vrefp_fast_(va));
    for (int k = 0; k < 4; ++k) FuzzNote(re, e[k], f[k], a[k], 0, 0);
    simde_mm_store_ps(reinterpret_cast<float*>(e), simde_mm_add_ps(simde_mm_mul_ps(va, vb), vc));
    simde_mm_store_ps(reinterpret_cast<float*>(f), rex_vmaddfp_fused_(va, vb, vc));
    for (int k = 0; k < 4; ++k) FuzzNote(ma, e[k], f[k], a[k], b[k], c[k]);
    // sasrt-nx (2026-11-11 ~10:00, the user: "add these please"): vnmsubfp fused and the vlogefp estimate.
    simde_mm_store_ps(reinterpret_cast<float*>(e),
                      simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(va, vb), vc),
                                      simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))));
    simde_mm_store_ps(reinterpret_cast<float*>(f), rex_vnmsubfp_fused_(va, vb, vc));
    for (int k = 0; k < 4; ++k) FuzzNote(nm, e[k], f[k], a[k], b[k], c[k]);
    simde_mm_store_ps(reinterpret_cast<float*>(e), rex_vlogefp_exact_(va));
    simde_mm_store_ps(reinterpret_cast<float*>(f), rex_vlogefp_fast_(va));
    for (int k = 0; k < 4; ++k) FuzzNote(lg, e[k], f[k], a[k], 0, 0);
  }
  __asm__ __volatile__("msr fpcr, %0" ::"r"(fpcr) : "memory");
  for (const auto& [name, st] : {std::pair<const char*, const FuzzStat&>{"vrsqrtefp", rs}, {"vrefp", re},
                                 {"vmaddfp (fused)", ma}, {"vnmsubfp (fused)", nm}, {"vlogefp", lg}})
    REXLOG_INFO("[vmxfuzz] {}: {} lanes, {} differ ({:.3f} %), {} in a special case (NaN / infinity / sign); the "
                "largest difference {} ulp (inputs {:#010x} {:#010x} {:#010x}), {:.3g} absolute (input {:#010x})",
                name, st.lanes, st.differ, st.lanes ? 100.0 * double(st.differ) / double(st.lanes) : 0.0, st.special,
                st.max_ulp, st.worst_in[0], st.worst_in[1], st.worst_in[2], st.max_abs, st.worst_abs_in);
}
}  // namespace
#endif
namespace {
// Port 72: the fuzz test's thread, once (sasrt_test_vmx_fuzz): from the first hook's InitHook, and (port 81) in place,
// where no hook runs, from the start-up notes (InPlaceNotes).
void StartVmxFuzzOnce() {
#if defined(REX_VMX_FLOAT_HELPERS)
  static std::once_flag fuzz_once;
  if (const int m = REXCVAR_GET(sasrt_test_vmx_fuzz); m > 0)
    std::call_once(fuzz_once, [m] {
      StartBackground([](void* p) { VmxFuzz(int(reinterpret_cast<intptr_t>(p))); }, reinterpret_cast<void*>(intptr_t(m)));
    });
#endif
}
}  // namespace
// sasrt-nx (2026-11-12, banjo_hot_poison_dead; port 121).
std::atomic<bool> g_poison_dead{false};

void PoisonDead(PPCContext& ctx, const RegSkip& skip) {
  // Cheap per call (it runs after every unguarded copy call): only the set bits are visited, and the one-time log line
  // uses a plain flag (a shared counter bumped on every call slowed sasrt-nx's first Ryujinx run about 4x).
  static std::atomic<bool> told{false};
  if (!told.load(std::memory_order_relaxed)) [[unlikely]] {
    if (!told.exchange(true, std::memory_order_relaxed))
      REXLOG_INFO("[hot] banjo_hot_poison_dead: the copies' dead-after-return registers get garbage from now on");
  }
  for (uint32_t m = skip.gpr; m; m &= m - 1) {
    const int i = __builtin_ctz(m);
    const_cast<PPCRegister*>(Gpr(ctx, i))->u64 = 0xBAD0BAD0BAD00000ull | uint64_t(i);
  }
  for (uint32_t m = skip.fpr; m; m &= m - 1) {
    const int i = __builtin_ctz(m);
    const_cast<PPCRegister*>(Fpr(ctx, i))->u64 = 0x7FF4BAD0BAD00000ull | uint64_t(i);
  }
  for (int w = 0; w < 2; ++w)
    for (uint64_t m = skip.vr[w]; m; m &= m - 1) {
      const int i = w * 64 + __builtin_ctzll(m);
      auto* v = const_cast<PPCVRegister*>(Vr(ctx, i));
      for (int k = 0; k < 4; ++k) v->u32[k] = 0x7FA0BA00u | uint32_t(i);
    }
}

uint8_t InitHook(Hook& h, uint8_t* base) {
  StartVmxFuzzOnce();
  g_poison_dead.store(REXCVAR_GET(banjo_hot_poison_dead), std::memory_order_relaxed);  // sasrt-nx (2026-11-12; port 121)
  // May run on several threads at once for the first call: every one of them stores the same values.
  bool on = banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, h.own());
  if (on && h.bulk && !REXCVAR_GET(banjo_cpu_native_bulk)) on = false;
  if (on && !GuestCodeMatches(h, base)) on = false;
  const bool verify = REXCVAR_GET(banjo_cpu_verify);
  uint32_t period = 1;
  while (period < uint32_t(std::max(1, REXCVAR_GET(banjo_hot_guard_period))) && period < (1u << kCountdownBits))
    period <<= 1;
  h.verify = verify;
  h.guard_calls = uint32_t(std::clamp(REXCVAR_GET(banjo_hot_guard_calls), 0, 4000));
  h.period = period;
  // No guard at all: the hook calls its copy directly (banjo_hot_guest.h, kDirect). sasrt-nx (2026-11-10, port 78; its
  // idea 27): also when the build has no guard forms (BANJO_HOT_GUARD_FORMS 0: the logged forms are not made), whatever
  // the keys ask. A key that asks for the guard gets one warning; the hooks' lines and the summary say it too.
  const bool forms = h.native_logged && h.reference_logged;
  const bool guard_asked = verify || h.guard_calls || REXCVAR_GET(banjo_hot_guard_period);
  if (!forms && guard_asked) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true, std::memory_order_relaxed))
      REXLOG_WARN("[hot] this build has no guard forms (SASRT_HOT_GUARD_FORMS OFF): banjo_cpu_verify, "
                  "banjo_hot_guard_calls and banjo_hot_guard_period check no native copy, every native copy is called "
                  "directly (an NRO with them: tools/build_switch_guard.bat; a PC build: -DSASRT_HOT_GUARD_FORMS=ON)");
  }
  const bool direct = !forms || !guard_asked;
  uint8_t state = on ? (direct ? kDirect : kOn) : kOff;
  if (on && REXCVAR_GET(banjo_hot_count) && h.native_counted) state = kCount;
  if (on && h.bulk) {
    // The bulk hooks say so in the summary of the reports (hundreds of first calls would each be a line).
  } else if (on && state == kDirect && h.state.load(std::memory_order_relaxed) == kUninit) {
    REXLOG_INFO("[hot] {} ({}): native copy ON, direct ({})", h.name, h.key,
                forms ? "no guard: banjo_hot_guard_calls = banjo_hot_guard_period = 0"
                      : "this build has no guard forms: SASRT_HOT_GUARD_FORMS OFF");  // port 78, its idea 27
  } else if (on && h.state.load(std::memory_order_relaxed) == kUninit) {
    if (state == kCount) {
      REXLOG_INFO("[hot] {} ({}): native copy ON in its counting form (banjo_hot_count; no guard)", h.name, h.key);
    } else if (verify) {
      REXLOG_INFO("[hot] {} ({}): native copy ON, verify mode (every call runs both and compares)", h.name, h.key);
    } else {
      REXLOG_INFO("[hot] {} ({}): native copy ON (guard: the first {} calls of a thread, then 1 in {})", h.name, h.key,
                  h.guard_calls, period);
    }
  }
  h.state.store(state, std::memory_order_release);
  return state;
}

void Counted(Hook& h, PPCContext& ctx, uint8_t* base) {
  h.native_counted(ctx, base);
  if ((h.uses.fetch_add(1, std::memory_order_relaxed) & 0xFFF) == 0) Report();
}

namespace {

enum class Outcome { kEqual, kDifferent, kNotComparable, kPathsDiffer };

// One pass of the guard (the steps at the top of this file). kEqual and kDifferent: the recompiled original has run
// for real and `diff` says what differed. kNotComparable / kPathsDiffer: nothing has run for real yet.
Outcome Attempt(Hook& h, PPCContext& ctx, uint8_t* base, Scratch& t, std::string& diff) {
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  t.ranges.clear();
  t.in = ctx;

  // 1 and 2: what would be written? Both logged forms, each rolled back.
  PPCContext logged_native = ctx;
  t.log.Reset(base);
  {
    Logged s{t.log};
    h.native_logged(logged_native, base, s);
  }
  const bool native_stopped = t.log.stopped;
  bool overflow = t.log.overflow;
  AddRanges(t.log, t.ranges);
  static const bool prefix = REXCVAR_GET(banjo_hot_guard_prefix);  // port 74, its idea 35
  static thread_local Written native_written, reference_written;
  if (prefix && native_stopped && !t.log.overflow) CaptureWritten(t.log, base, native_written);
  t.log.RollBack();
  ctx.fpscr.setcsr(hw_before);

  PPCContext logged_reference = ctx;
  t.log.Reset(base);
  {
    Logged s{t.log};
    h.reference_logged(logged_reference, base, s);
  }
  const bool reference_stopped = t.log.stopped;
  overflow |= t.log.overflow;
  AddRanges(t.log, t.ranges);
  if (prefix && reference_stopped && !overflow) CaptureWritten(t.log, base, reference_written);
  t.log.RollBack();
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;

  if (native_stopped || reference_stopped) {
    // A call the guard cannot repeat, or a log past its limit. One of the two stopping and the other not means
    // they took different paths.
    if (native_stopped != reference_stopped && !overflow) {
      diff = fmt::format("the copy {} a call outside itself and the original statements {}",
                         native_stopped ? "reached" : "did not reach", reference_stopped ? "did" : "did not");
      return Outcome::kPathsDiffer;
    }
    if (prefix && native_stopped && reference_stopped && !overflow) {  // port 74, its idea 35
      std::string where;
      if (PrefixEqual(base, native_written, reference_written, where)) {
        h.prefix_equal.fetch_add(1, std::memory_order_relaxed);
      } else {
        const uint64_t n = h.prefix_differ.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 2)
          REXLOG_WARN("[hot] {}: the stores before the stop differ (banjo_hot_guard_prefix; not acted on): {}",
                      h.name, where);
      }
      TrimWritten(native_written);
      TrimWritten(reference_written);
    }
    return Outcome::kNotComparable;
  }

  // 3: the compare set, saved; the real copy on a copy of the registers; its bytes out, the saved ones back.
  Merge(t.ranges);
  size_t total = 0;
  for (const Region& r : t.ranges) total += r.len;
  t.bytes = total;
  t.before.resize(total);
  t.native_out.resize(total);
  size_t off = 0;
  for (const Region& r : t.ranges) {
    std::memcpy(t.before.data() + off, Raw(base, r.addr), r.len);
    off += r.len;
  }
  PPCContext nat = ctx;
  h.native(nat, base);
  const uint32_t hw_native = nat.fpscr.getcsr();
  off = 0;
  for (const Region& r : t.ranges) {
    std::memcpy(t.native_out.data() + off, Raw(base, r.addr), r.len);
    std::memcpy(Raw(base, r.addr), t.before.data() + off, r.len);
    off += r.len;
  }
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;

  // 4: the call for real. (It may contain guarded calls of its own: they use the next scratch frame.)
  h.orig(ctx, base);
  const uint32_t hw_orig = ctx.fpscr.getcsr();

  // 5: compare.
  diff = CompareRegisters(nat, ctx, *h.skip);
  if (diff.empty() &&
      (hw_native & rex::ppc::FPSCRRegister::GuestMask) != (hw_orig & rex::ppc::FPSCRRegister::GuestMask))
    diff = fmt::format("host flush mode copy={:#x} original={:#x}", hw_native, hw_orig);
  if (diff.empty()) {
    off = 0;
    for (const Region& r : t.ranges) {
      const uint8_t* now = Raw(base, r.addr);
      if (std::memcmp(now, t.native_out.data() + off, r.len) != 0) {
        uint32_t k = 0;
        while (now[k] == t.native_out[off + k]) ++k;
        diff = fmt::format("memory {:#010x} (+{} of the written range {:#010x}+{}) copy={:#04x} original={:#04x}",
                           r.addr + k, k, r.addr, r.len, t.native_out[off + k], now[k]);
        break;
      }
      off += r.len;
    }
  }
  if (diff.empty()) {
    diff = CompareRegisters(logged_native, ctx, *h.skip);
    if (!diff.empty()) diff = "the copy's logged form: " + diff;
  }
  if (diff.empty()) {
    // (The skip set here too: the recompiled original that just ran reaches the hand-written natives through the
    // hooks of their functions, so it has their registers, not the original statements'.)
    diff = CompareRegisters(logged_reference, ctx, *h.skip);
    if (!diff.empty()) diff = "the original statements' logged form: " + diff;
  }
  return diff.empty() ? Outcome::kEqual : Outcome::kDifferent;
}

}  // namespace

namespace {
// blur-nx (2026-11-16): banjo_hot_guard_keep: is this hook's function on the list? (The list read once.)
bool KeptByGuard(const Hook& h) {
  static const std::vector<std::string> list = [] {
    std::vector<std::string> out;
    const std::string text = REXCVAR_GET(banjo_hot_guard_keep);
    size_t at = 0;
    while (at <= text.size()) {
      size_t comma = text.find(',', at);
      if (comma == std::string::npos) comma = text.size();
      std::string item = text.substr(at, comma - at);
      while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
      while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
      if (item.rfind("sub_", 0) == 0) item = item.substr(4);
      for (char& c : item) c = char(std::toupper(static_cast<unsigned char>(c)));
      if (!item.empty()) out.push_back(item);
      at = comma + 1;
    }
    if (!out.empty()) REXLOG_INFO("[hot] banjo_hot_guard_keep: {} function(s) the guard never switches off", out.size());
    return out;
  }();
  if (list.empty() || !h.name) return false;
  std::string name = h.name;
  if (name.rfind("sub_", 0) == 0) name = name.substr(4);
  for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
  for (const std::string& item : list)
    if (item == name) return true;
  return false;
}
}  // namespace

void Check(Hook& h, PPCContext& ctx, uint8_t* base, uint32_t& counter) {
  // The next guarded call of this thread: at once while it is still in its first guard_calls (or in verify mode),
  // else after `period` calls. The counter word: banjo_hot_guest.h.
  const uint32_t phase = counter >> kCountdownBits;
  if (h.verify) {
    counter = 0;
    h.uses.fetch_add(1, std::memory_order_relaxed);
  } else if (phase < h.guard_calls) {
    counter = (phase + 1) << kCountdownBits;
    h.uses.fetch_add(1, std::memory_order_relaxed);
  } else {
    counter = (phase << kCountdownBits) | (h.period - 1);
    h.uses.fetch_add(h.period, std::memory_order_relaxed);
  }
  Report();  // at most every 10 s
  if (h.state.load(std::memory_order_relaxed) != kOn) {  // switched off by another thread's check meanwhile
    h.orig(ctx, base);
    return;
  }

  ScratchFrame frame(Thread());
  Scratch& t = frame.scratch;
  const uint32_t hw_entry = ctx.fpscr.getcsr();
  std::string diff;
  // A difference must repeat to count. The copy and the original are functions of the registers and of guest
  // memory; when another thread changes memory they read between the guard's runs (the audio threads read buffers
  // the decoder is filling), the runs differ without either being wrong. So on a difference the inputs are put back
  // (the registers the call came with, the bytes it wrote) and the whole pass is made again, twice at most: a wrong
  // copy differs every time, a race does not.
  constexpr int kAttempts = 3;
  for (int attempt = 1;; ++attempt) {
    const Outcome outcome = Attempt(h, ctx, base, t, diff);
    if (outcome == Outcome::kNotComparable) {
      h.not_comparable.fetch_add(1, std::memory_order_relaxed);
      if (h.verify) {
        h.orig(ctx, base);
      } else {
        h.native(ctx, base);
      }
      return;
    }
    if (outcome == Outcome::kEqual) {
      h.compared.fetch_add(1, std::memory_order_relaxed);
      ::me::native::g_verify_native_checks.fetch_add(1, std::memory_order_relaxed);  // the "[verify]" line
      if (attempt > 1) h.not_repeated.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (attempt < kAttempts) {
      if (outcome == Outcome::kDifferent) {  // the original has run: put its inputs back
        ctx = t.in;
        size_t off = 0;
        for (const Region& r : t.ranges) {
          std::memcpy(Raw(base, r.addr), t.before.data() + off, r.len);
          off += r.len;
        }
        ctx.fpscr.setcsr(hw_entry);
      }
      continue;
    }
    // It repeated.
    h.compared.fetch_add(1, std::memory_order_relaxed);
    ::me::native::g_verify_native_checks.fetch_add(1, std::memory_order_relaxed);
    const uint64_t nth = h.differences.fetch_add(1, std::memory_order_relaxed) + 1;
    if (KeptByGuard(h)) {
      // blur-nx (2026-11-16): on banjo_hot_guard_keep: logged (the first three of this hook), the copy stays on.
      if (nth <= 3)
        REXLOG_WARN("[hot] difference in {} ({}), kept on (banjo_hot_guard_keep): {}", h.name, h.key, diff);
      if (outcome == Outcome::kPathsDiffer) h.orig(ctx, base);  // nothing has run for real yet
      return;
    }
    h.state.store(kOff, std::memory_order_relaxed);
    ::me::native::VerifyDifference();
    const PPCContext& in = t.in;
    REXLOG_ERROR("[hot] DIFFERENCE {} ({}): {} ({} passes in a row); inputs r3={:#x} r4={:#x} r5={:#x} r6={:#x} "
                 "r7={:#x} f1={:#x} f2={:#x}; {} bytes in {} written ranges; native copy switched off for the rest of "
                 "the run",
                 h.name, h.key, diff, kAttempts, in.r3.u64, in.r4.u64, in.r5.u64, in.r6.u64, in.r7.u64, in.f1.u64,
                 in.f2.u64, t.bytes, t.ranges.size());
    if (outcome == Outcome::kPathsDiffer) h.orig(ctx, base);  // nothing has run for real yet
    return;
  }
}

#if BANJO_HOT_IN_PLACE
namespace {
// sasrt-nx (2026-11-11, port 81; machine copies in place): the in-run A/B's target 70
// (banjo_renut_ab_target, or in the list banjo_renut_ab_targets; read by name: Renut's renderer defines them).
bool AbAsks70() {
  if (rex::cvar::Query<int32_t>("banjo_renut_ab_target") == 70) return true;
  int n = 0;
  bool digit = false;
  for (const char c : rex::cvar::Query<std::string>("banjo_renut_ab_targets") + ",") {
    if (c >= '0' && c <= '9') {
      n = n * 10 + (c - '0');
      digit = true;
    } else {
      if (digit && n == 70) return true;
      n = 0;
      digit = false;
    }
  }
  return false;
}

// No hook reads a key in this build, so once, at start-up (ForEachKey, for the "[enhancements]" line; or the first
// Report), the log says what the build fixed and names each key that asks for what only a hooked build does, as port
// 78 does for the guard forms. The hand-written natives keep their keys (their own state byte inside the copies), and
// so do the in-run A/B's target 69 and the transform chain's own check (katamari_hand_transform_chain_verify).
void InPlaceNotes() {
  static std::atomic<bool> said{false};
  if (said.exchange(true, std::memory_order_relaxed)) return;
  StartVmxFuzzOnce();  // port 72's test: no InitHook starts it in place
  uint32_t on = 0, bulk_on = 0, kept = 0, against = 0;
  std::string keys;
  for (const InPlaceCopy* c = g_in_place_list; c; c = c->next) {
    on += c->on;
    bulk_on += c->on && c->bulk;
    kept += !c->on;
    if (c->own() != c->on) {
      ++against;
      if (against <= 8) keys += fmt::format("{}{} = {}", against > 1 ? ", " : "", c->key, c->on ? "false" : "true");
    }
  }
  REXLOG_INFO("[hot] native copies IN PLACE (SASRT_HOT_IN_PLACE ON): {} copies are their generated functions ({} of "
              "them bulk), {} marked off in the lists stay generated code; no hook: no per-copy key, no guard, no "
              "counts, no in-run A/B 70 (all of them in a hooked build: tools/build_switch_guard.bat, IN_PLACE=OFF)",
              on, bulk_on, kept);
  if (against)
    REXLOG_WARN("[hot] {} native-copy key(s) set against this build, which does not read them (the copies are in "
                "place: localized.txt and localized_bulk.txt decide, `off` keeps a function generated code): {}{}",
                against, keys, against > 8 ? ", ..." : "");
  if (bulk_on && !REXCVAR_GET(banjo_cpu_native_bulk))
    REXLOG_WARN("[hot] banjo_cpu_native_bulk = false does nothing in this build: its {} bulk copies are in place "
                "(SASRT_HOT_IN_PLACE ON; a hooked build switches them with it)",
                bulk_on);
  if (on && !banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, true))
    REXLOG_WARN("[hot] banjo_cpu_enhancements = false leaves the {} native copies on in this build (in place, "
                "SASRT_HOT_IN_PLACE ON); it still switches the hand-written natives and the other CPU items off",
                on);
  if (REXCVAR_GET(banjo_cpu_verify) || REXCVAR_GET(banjo_hot_guard_calls) || REXCVAR_GET(banjo_hot_guard_period))
    REXLOG_WARN("[hot] this build has the native copies in place (SASRT_HOT_IN_PLACE ON: no hooks, no guard forms): "
                "banjo_cpu_verify, banjo_hot_guard_calls and banjo_hot_guard_period check no native copy (a guard or "
                "verify NRO: tools/build_switch_guard.bat; a PC build: -DSASRT_HOT_IN_PLACE=OFF "
                "-DSASRT_HOT_GUARD_FORMS=ON)");
  if (REXCVAR_GET(banjo_hot_count))
    REXLOG_WARN("[hot] banjo_hot_count counts nothing in this build: the native copies are in place "
                "(SASRT_HOT_IN_PLACE ON), and only a hook runs a copy's counting form");
  if (AbAsks70())
    REXLOG_WARN("[hot] the in-run A/B's target 70 has no off half in this build: the native copies are in place "
                "(SASRT_HOT_IN_PLACE ON) and nothing calls the generated originals; both halves run the copies (a "
                "hooked build: IN_PLACE=OFF, -DSASRT_HOT_IN_PLACE=OFF)");
}
}  // namespace
#endif

void ForEachKey(void (*row)(void* user, const char* key, bool own), void* user) {
  bool bulk = false;
#if BANJO_HOT_IN_PLACE
  // Port 81: each copy as the build made it (its list's choice; the key is not read, InPlaceNotes names one set
  // against it), and the bulk copies' master as on (it cannot switch them off).
  InPlaceNotes();
  for (const InPlaceCopy* c = g_in_place_list; c; c = c->next) {
    if (!c->bulk) {
      row(user, c->key, c->on);
    } else {
      bulk = true;
      if (!c->on) row(user, c->key, false);  // marked off in the bulk list: generated code
    }
  }
  if (bulk) row(user, "banjo_cpu_native_bulk", true);
#else
  for (Hook* h = g_hooks; h; h = h->next) {
    if (!h->bulk) {
      row(user, h->key, h->own());
    } else {
      bulk = true;
      if (!h->own()) row(user, h->key, false);  // a bulk copy switched off by its own key
    }
  }
  if (bulk) row(user, "banjo_cpu_native_bulk", REXCVAR_GET(banjo_cpu_native_bulk));
#endif
  for (uint32_t k = 0; k < ::banjo::hand::kNatives; ++k) row(user, ::banjo::hand::kKeys[k], ::banjo::hand::Own(k));
}

void Report() {
#if BANJO_HOT_IN_PLACE
  // Port 81: nothing is counted in place (no hook: no guard, no counting form). The start-up notes said what the build
  // has; they come here if the "[enhancements]" line never ran.
  InPlaceNotes();
#else
  const int64_t now = NowMs();
  int64_t last = g_last_report_ms.load(std::memory_order_relaxed);
  if (now - last < 10000 || !g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
  if (REXCVAR_GET(banjo_hot_count)) {
    WriteCounts();
    // The hand-written natives' calls by size (for an estimate of what they cost on the A57).
    for (uint32_t k = 0; k < ::banjo::hand::kNatives; ++k) {
      std::string sizes;
      uint64_t calls = 0;
      for (uint32_t b = 0; b < 34; ++b) {
        const uint64_t n = ::banjo::hand::g_sizes[k][b].load(std::memory_order_relaxed);
        if (!n) continue;
        calls += n;
        sizes += b == 0 ? fmt::format(" [0: {}]", n) : fmt::format(" [{}-{}: {}]", 1u << (b - 1), (1ull << b) - 1, n);
      }
      if (calls)
        REXLOG_INFO("[hot] hand-written native of the {}: {} calls so far, by size ({}):{}", ::banjo::hand::kNames[k],
                    calls,
                    k == ::banjo::hand::kConstants ? "vectors" : k == ::banjo::hand::kFlush ? "words written" : "bytes",
                    sizes);
    }
    return;
  }
  // One line per hook that was reached, so each fits the log's line and a tool reads one function per line.
  uint64_t compared = 0, not_comparable = 0, differences = 0, not_repeated = 0, prefix_equal = 0, prefix_differ = 0;
  uint32_t reached = 0, on = 0, direct = 0, bulk_total = 0, bulk_reached = 0, bulk_on = 0;
  // banjo_hot_bulk_lines: every 6th report (about once a minute), so a long run's log stays small.
  static uint32_t report_number = 0;
  const bool bulk_lines = REXCVAR_GET(banjo_hot_bulk_lines) && (report_number++ % 6) == 0;
  for (Hook* h = g_hooks; h; h = h->next) {
    const uint8_t state = h->state.load(std::memory_order_relaxed);
    bulk_total += h->bulk;
    prefix_equal += h->prefix_equal.load(std::memory_order_relaxed);  // port 74, its idea 35 (every hook)
    prefix_differ += h->prefix_differ.load(std::memory_order_relaxed);
    if (state == kUninit) continue;
    ++reached;
    on += state == kOn || state == kDirect;
    direct += state == kDirect;
    if (h->bulk) {
      ++bulk_reached;
      bulk_on += state == kOn || state == kDirect;
      compared += h->compared.load(std::memory_order_relaxed);
      not_comparable += h->not_comparable.load(std::memory_order_relaxed);
      not_repeated += h->not_repeated.load(std::memory_order_relaxed);
      const uint64_t d = h->differences.load(std::memory_order_relaxed);
      differences += d;
      // A bulk hook has a line of its own only when it found a difference (its copy is off since).
      if (d)
        REXLOG_INFO("[hot] {} (bulk) off after {} difference(s); {} calls compared before", h->name, d,
                    h->compared.load(std::memory_order_relaxed));
      // banjo-nx (2026-11-19, the rebase; a handoff): banjo_hot_bulk_lines, a line per reached bulk copy, so a verify
      // run shows which copies of a new list the guard reached (the guide: verified only with the counts showing it).
      else if (bulk_lines)
        REXLOG_INFO("[hot] bulk reached {} {}: {} compared, {} not comparable", h->name,
                    state == kOn || state == kDirect ? "on" : "off", h->compared.load(std::memory_order_relaxed),
                    h->not_comparable.load(std::memory_order_relaxed));
      continue;
    }
    const uint64_t c = h->compared.load(std::memory_order_relaxed);
    const uint64_t n = h->not_comparable.load(std::memory_order_relaxed);
    const uint64_t d = h->differences.load(std::memory_order_relaxed);
    compared += c;
    not_comparable += n;
    differences += d;
    const uint64_t r = h->not_repeated.load(std::memory_order_relaxed);
    not_repeated += r;
    if (state == kDirect) continue;  // nothing is counted without the guard: the summary says it is on
    REXLOG_INFO("[hot] {} {}: about {} calls so far, {} compared with the original, {} not comparable (they reach a "
                "call outside the copy), {} differences, {} more that did not repeat (inputs changed by another thread)",
                h->name, state == kOn ? "ON" : "off", h->uses.load(std::memory_order_relaxed), c, n, d, r);
  }
  REXLOG_INFO("[hot] native copies: {} reached, {} on ({} direct, no guard); bulk copies (banjo_cpu_native_bulk {}): {} "
              "of {} reached, {} on; {} calls compared so far, {} not comparable ({} of them stopped in both forms "
              "with the same stores before the stop, {} not), {} differed once and not when repeated, differences: "
              "{} (registers that differed only in a NaN's sign, taken as the same: {}){}",
              reached, on, direct, REXCVAR_GET(banjo_cpu_native_bulk) ? "on" : "off", bulk_reached, bulk_total, bulk_on,
              compared, not_comparable, prefix_equal, prefix_differ, not_repeated, differences,
              g_guard_nan_sign_only.load(std::memory_order_relaxed),  // blur-nx (2026-11-13, #69)
              // port 78, its idea 27: a build without the guard's forms compares nothing, whatever the keys say.
              BANJO_HOT_GUARD_FORMS ? "" : " (this build has no guard forms, SASRT_HOT_GUARD_FORMS OFF: nothing compared)");
#endif  // BANJO_HOT_IN_PLACE
}

}  // namespace banjo::hot

// The hooks (tools/localize_native.py writes one BANJO_HOT_HOOK line per function of localized.txt).
#include "hot/banjo_hot_list.inc"
