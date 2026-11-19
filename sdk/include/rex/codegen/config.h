/**
 * @file        rexcodegen/internal/config.h
 * @brief       Recompiler configuration types
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/codegen/function_graph.h>  // For JumpTable

namespace rex::codegen {

struct MidAsmHook {
  std::string name;
  std::vector<std::string> registers;

  bool ret = false;
  bool returnOnTrue = false;
  bool returnOnFalse = false;

  uint32_t jumpAddress = 0;
  uint32_t jumpAddressOnTrue = 0;
  uint32_t jumpAddressOnFalse = 0;

  bool afterInstruction = false;
};

// Unified function/chunk configuration
// A "chunk" is simply a function entry with a non-zero parent field
struct FunctionConfig {
  uint32_t size = 0;    // Explicit size in bytes (mutually exclusive with end)
  uint32_t end = 0;     // End address, exclusive (mutually exclusive with size)
  std::string name;     // Custom symbol name (empty = auto-generate sub_XXXXXXXX)
  uint32_t parent = 0;  // Parent function address (0 = standalone, non-zero = chunk)

  // Keep non-volatiles in ctx instead of localizing them. Marks an MSVC SEH
  // funclet, which reads the registers its owner left live and would see a zero
  // from a local. Callers sync their localized copies across the call site.
  bool shareRegisters = false;

  // Get effective size (prefers size over end)
  uint32_t getSize(uint32_t address) const {
    return size ? size : (end > address ? end - address : 0);
  }
  // Returns true if this is a discontinuous chunk belonging to a parent function
  bool isChunk() const { return parent != 0; }
};

// Section info for analysis output
struct SectionInfo {
  std::string name;
  uint64_t address = 0;
  uint64_t size = 0;
  std::string flags;  // "rx", "rw", "r" etc.
};

// Function entry for analysis output
struct FunctionEntry {
  uint64_t address = 0;
  uint64_t size = 0;
  std::string name;  // optional, defaults to "sub_XXXXXXXX"
};

struct RecompilerConfig {
  // === Required user-provided fields ===
  std::string projectName = "rex";  ///< Project name for output files
  std::string filePath;             ///< Path to XEX/ELF file
  std::string outDirectoryPath;     ///< Output directory for generated code
  std::string templateDir;          ///< Optional custom template directory for overrides

  /// Every TOML that fed this config, in load order. LoadFromTable() gets a
  /// parsed table, so a manifest-embedded entry omits the manifest itself.
  std::vector<std::string> loadedFiles;

  // === Code generation options (optional) ===
  bool skipLr = false;
  /// Mass Effect diet: `ctx.lr = <return address>;` stores before bl/bctrl/blrl are dropped (nothing but a stack
  /// slot reads the guest LR), except at the return addresses listed in lrKeepReturns (hooks that compare
  /// ctx.lr) and at PIC `bl` to a local label (the LR is then the data).
  bool elideLrStores = false;
  /// Mass Effect diet: D-form loads/stores pass the register and the displacement apart
  /// (REX_LOAD_D_*(reg, disp) = base + zext(reg) + disp) instead of (u32)(reg + disp).
  bool dformDispSplit = false;
  /// Mass Effect diet: fctiwz as an fcvtzs plus a NaN select (rex::ppc::fctiwz).
  bool fctiwzInline = false;
  /// sa2-nx round 7: no host flush-mode switch (an `msr fpcr` on AArch64, very slow on the A57) for floating-point
  /// moves: lfd/stfd and their indexed/update forms, stfiwx, fmr, fabs, fnabs and fneg only move bits. And in VMX
  /// mode (flush on), lfs/stfs and their forms convert through rex::ppc::SingleToDoubleAnyMode /
  /// DoubleToSingleAnyMode, which give the IEEE result in either mode, instead of switching the mode off and on again.
  bool fpMovesModeless = false;
  // sasrt-nx (2026-10-12): the scalar float instructions whose result the flush mode cannot change switch nothing
  // (fcfid, fctiwz, fctidz, fsel), and fcmpu / fcmpo / frsp go through any-mode helpers while VMX mode is current.
  bool fpComparesModeless = false;
  // sasrt-nx (2026-10-12): before a scalar float instruction switches the flush mode, the generator looks at the
  // next instruction that needs a particular mode (BuilderContext::next_float_demand). The any-mode forms are then
  // used whenever the mode is not known to be FPU and vector code (or nothing) follows, and one or two scalar single
  // operations between vector instructions compute in the current mode (rex::ppc::*SingleAnyMode).
  bool fpModeLookahead = false;
  // sa2-nx (2026-10-12): with fpModeLookahead, whether a conversion or compare also takes its any-mode form when
  // nothing that needs a mode follows before the next branch, in a function with vector float code or a "mode-free"
  // one (sasrt-nx's rule: true). It bets a test in every such conversion against a switch that may come later. In a
  // game whose float code is mostly scalar the bet loses: SA2 ran 4.5-8.7 million any-mode conversions a second
  // (PC census, REX_DIET_CENSUS_ON) to save at most 180 switches a frame. false keeps the any-mode forms where they
  // replace a switch for certain: a vector instruction comes next, or VMX mode is known to be current.
  bool fpModeLookaheadPastBranch = true;
  // sasrt-nx (2026-10-12): rlwinm / rlwnm / rlwimi whose mask does not wrap (mb <= me) are written as a 32-bit
  // rotate and a 32-bit mask instead of a 64-bit rotate of the word duplicated into both halves. Only a wrapping
  // mask reaches the upper half, so the result is the same; the compiler then sees a plain shift, bit-field
  // extract or AND (one instruction instead of three on AArch64).
  bool rotateWordNarrow = false;
  // banjo-nx (2026-10-16, measured by its CPU codegen agent): rlwinm / rlwimi with an immediate shift and a mask that
  // does not wrap (mb <= me) written as the plain 32-bit operation the mask makes of it: a left shift when the mask
  // takes only bits the left shift supplies, a right shift when it takes only bits the right shift supplies, a 32-bit
  // rotate otherwise (and an AND when the mask is narrower). GCC 16 makes lsl / lsr / ubfiz / ubfx of those (one
  // instruction) where the 64-bit rotate of the duplicated word took two or three; rotate_word_narrow's rotate-and-mask
  // form did not get there with the Switch compiler. Same zero-extended value (sdk/tests/rotate_word_test.cpp, every
  // shift and mask). Takes precedence over rotate_word_narrow for rlwinm / rlwimi; rlwnm (a register shift) unchanged.
  bool rotateWordShifts = false;
  // sa2-nx (second CPU round, 2026-10-12): fctid, fctiw and fctidz as rex::ppc::fctid / fctiw / fctidz
  // (rex/ppc/diet_ops.h) instead of an isnan test, a range compare and simde's scalar conversion around each. On
  // AArch64 they are `fcvtzs` (after `frinti` for the two that round) and a NaN select. For fctid and fctiw this
  // also CORRECTS the AArch64 build: simde's portable conversion rounds with C round() (halves away from zero),
  // the Xbox 360 and the x86 build round in the current mode (halves to even).
  bool fctiInline = false;
  // sa2-nx (second CPU round): a float copied through a register (lfs / lfsx, then stfs / stfsx of the same
  // register: structure and matrix copies) needs no flush mode. The load keeps its bits in a local of that register
  // (fb0..fb31) and gives the register its value through rex::ppc::SingleToDoublePure (integer arithmetic for a
  // denormal: no mode, and the compiler drops it when the register is not read again); a store whose register
  // still holds what such a load put there writes those bits with a signalling NaN quieted
  // (rex::ppc::QuietSingleBits), which is what the two conversions gave. "Still holds": going back from the store
  // in its block, the first instruction that writes the register is that load, and everything between is another
  // lfs / lfsx / stfs / stfsx or an instruction that touches no float or vector register and is no branch, call or
  // trap; no label after the load up to the store, no mid-asm hook from the load to the store.
  // Used where VMX mode is known to be current: there the plain forms would switch the mode off and back, and the
  // any-mode helpers they otherwise go through cost a test and a conversion each.
  bool fpCopyPairs = false;
  // banjo-nx (2026-10-19, CPU proposal 15): with fpCopyPairs, a store of loaded bits whatever the mode, and through
  // other instructions: going back from the stfs / stfsx in its block, the first instruction that writes its register
  // must be the lfs / lfsx of it or an fneg / fabs / fnabs of the register in place (bit 31 of the bits flipped,
  // cleared or set: the double's sign is the single's), and nothing between is a branch, call or trap (other
  // instructions, float and vector ones too, may stand there: they write other registers). An lfs / lfsx whose bits
  // such a store writes takes the copy form as well, so the bits local is always written: the two decisions are the
  // same test. The forms need no mode: SingleToDoublePure is exact in either, and the stored bits are what the plain
  // conversions give with the mode off (the culling loop of sub_829A6DE8: lfs, fneg, stfs paid two range checks and
  // two conversions).
  bool fpCopyForwarding = false;
  // sa2-nx (second CPU round): with fpCopyPairs, also in every copy-only function (no instruction that needs FPU
  // mode, every lfs / stfs form an lfs / lfsx or a store of loaded bits), whatever the mode: such a function then
  // has no mode statement at all, and a caller in VMX mode no longer pays a switch into it and one back. A BET, and
  // off by default: the copy forms cost a test per load and per store on every call, the switches are saved only
  // on the calls that come from vector code. SA2 (PC census, 2026-10-12): 5.25 million copy forms a second to
  // save 22,000 switches a second, 235 forms per switch; its matrix copies run 1,500 times a frame, one call in
  // seven from vector code. Worth it only where copies are mostly called from vector code.
  bool fpCopyOnlyFunctions = false;
  // sa2-nx (second CPU round): dcbt / dcbtst as REX_PREFETCH (the host's prefetch of the same address) instead
  // of nothing. A hint: no effect on any value.
  bool dcbtPrefetch = false;
  // sa2-nx (third CPU round, 2026-10-12): divw / divwu / divd / divdu through rex::ppc::divw ... (rex/ppc/diet_ops.h)
  // instead of the guarded C division. The guards exist because dividing by zero (and INT_MIN / -1) is undefined in
  // C; AArch64's sdiv / udiv define both (0 for a zero divisor), so there the helper is the instruction, plus one
  // select for the signed overflow case, without a branch. Same results on every platform. A divisor the compiler
  // knows keeps the C expression (it turns that into a multiplication).
  bool divInline = false;
  // futari-nx (2026-10-26; port 55, from feedingfrenzy2-nx's SDK): VMX float results as the Xbox 360 gives them,
  // where the code generator wrote x86's (simde emulates x86 on AArch64 too, so the PC and the Switch builds agreed
  // with each other and not with the 360). Each its own key, so one can be put back. The helpers are in
  // rex/ppc/intrinsics.h. All off: the generated code is what it was.
  //   vmx_exact_minmax       vmaxfp / vminfp: a NaN operand gives that NaN quieted (vA's first), max(+0, -0) = +0
  //                          and min(+0, -0) = -0. x86's maxps / minps return the second operand for a NaN or two
  //                          zeros. AArch64: FMAX / FMIN, one instruction.
  //   vmx_fused_madd         vmaddfp / vmaddcfp128 / vnmsubfp fused (one rounding), as AltiVec's are; they were a
  //                          multiply and an add (two roundings; +max * 2 - +max overflowed). AArch64: FMLA.
  //   vmx_expte_range        vexptefp: 0 below -126 (the 360 flushes the denormal results), +inf from 128, a NaN
  //                          quieted; the polynomial's exponent arithmetic made garbage outside [-126, 128).
  //   vmx_cmpb_nan           vcmpbfp: a NaN operand sets both bound bits (out of bounds), as on the 360.
  //   vmx_dot_overflow_qnan  vmsum3fp128 / vmsum4fp128: QNaN where the sum of finite inputs overflows
  //                          (rexglue-sdk PR #492).
  bool vmxExactMinMax = false;
  bool vmxFusedMadd = false;
  bool vmxExpteRange = false;
  bool vmxCmpbNan = false;
  bool vmxDotOverflowQnan = false;
  // renut-ultimate (2026-11-16, port 144; tw2de_nx's fixes, found with Xenia's hardware-recorded instruction tests).
  // Each its own key, all off by default (the user: "make sure its off by default"): the generated code is what it was.
  //   vcmpbfp_cr6_exact      vcmpbfp.: CR6 is only "eq" (every element within its bounds); the mask test also set "lt"
  //                          when every element was out of bounds (the hardware gives CR6 = 0 there).
  //   vupkd3d_exact          vupkd3d128: NORMPACKED64 (4:20:20:20) from the guest's low doubleword with its 3.0 / 1.0
  //                          biases (it read the high one, dropped the bias and shifted an int32 by more than 31); the
  //                          most negative field a quiet NaN in NORMSHORT2, NORMSHORT4 and NORMPACKED32's y and z too,
  //                          as Xenia's x64 unpack (from nx-mod/rexglue-sdk-nx).
  //   fpscr_moves_exact      mtfsf's field mask in the PowerPC's order (FM 0x80 is field 0, FPSCR 0xF0000000: it was
  //                          reversed, so `mtfsf 1` did not set the rounding mode), mffs's upper word 0xFFF80000, and
  //                          builders for mtfsfi, mtfsb0, mtfsb1 and mcrfs (REX_UNIMPLEMENTED traps without).
  //   extra_instructions     builders for the instructions with none (builders/extra.cpp, instruction_dispatch.cpp:
  //                          vaddcuw ... vpkpx, lmw, ldbrx, stdbrx, rotld, mcrxr, mfvscr, mtvscr, isync, icbi, mtmsr,
  //                          lvsl128 / lvsr128, the LRU-hint vector loads and stores, and every OE form).
  bool vcmpbfpCr6Exact = false;
  bool vupkd3dExact = false;
  bool fpscrMovesExact = false;
  bool extraInstructions = false;
  // renut-ultimate (2026-11-16, port 144; tw2de_nx's, ON by default at the user's word: "port the speed ups as on by
  // default"): spin_hint_yield: the Xenon's spin-wait hints (db16cyc, or r1,r1,r1) call rex_spin_hint
  // (rex/ppc/context.h), which yields the core (the base's MaybeYield, with its backoff); they were nothing. false in
  // the game's codegen toml: nothing, as before.
  bool spinHintYield = true;
  // renut-ultimate (2026-11-16, port 144; tw2de_nx's, off by default): the console's own results where the host's
  // differ (rex/ppc/vmx_est360.h). vmx_360_estimates: vrefp, vrsqrtefp, vexptefp, vlogefp, fres and frsqrte from
  // the hardware's tables (Xenia's and xenia-edge's; the default is the exact division, square root, polynomial and
  // log2f; port 72's NEON estimates replace those, not these). vmx_dot_360: vmsum3fp128 / vmsum4fp128 with the
  // console's rounding (float64 sums truncated to single), slower than the float32 dot product.
  bool vmx360Estimates = false;
  bool vmxDot360 = false;
  // renut-ultimate (2026-11-16, port 144; tw2de_nx's of 2026-11-01, off by default): exact_fp: the FPU and vector float
  // results the Xbox 360 gives where the host's differ (rex/ppc/fpu_exact.h: the NaN of the first NaN operand in the
  // 360's order, quieted, QNaN 0x7FF8... for an invalid operation, the single-precision instructions with a double
  // denormal operand). A game's code with valid inputs never sees a difference; each helper adds a test of the result.
  // exact_fp_functions = [addr, ...]: exact_fp in these functions only. exact_fp_log (diagnostic): the host's result
  // kept, and every instruction exact_fp would change reported once with its function (the line to copy into
  // exact_fp_functions). fp_origin_log (diagnostic): every float instruction that makes a NaN or an infinity out of
  // finite operands reported, with the guest's callers (sdk/src/core/ppc_exact_log.cpp).
  bool exactFp = false;
  std::unordered_set<uint32_t> exactFpFunctions;
  bool exactFpLog = false;
  bool fpOriginLog = false;
  std::unordered_set<uint32_t> lrKeepReturns;
  bool ctrAsLocalVariable = false;
  bool xerAsLocalVariable = false;
  bool reservedRegisterAsLocalVariable = false;
  bool skipMsr = false;
  bool crRegistersAsLocalVariables = false;
  bool nonArgumentRegistersAsLocalVariables = false;
  /// non_argument_as_local_keep = [addr, ...] (sa2-nx, via peggle2-nx): functions that keep r0/r2/r11/r12/f0/v32-v63
  /// in ctx under non_argument_as_local: a callee that takes an argument there (the stack probe's frame size in r12),
  /// or a caller that passes one through an indirect call (the C++ EH dispatcher). Their callers hand the localized
  /// registers over (emit_call_handing_non_arguments). Found by tools/non_argument_audit.py on code generated without
  /// the option.
  std::unordered_set<uint32_t> nonArgumentKeep;
  bool nonVolatileRegistersAsLocalVariables = false;
  /// non_volatile_as_local_exclude = [[start, end], ...]: functions starting in [start, end) keep r14-r31,
  /// f14-f31, v14-v31 and v64-v127 in ctx and really call the __save*/__rest* helpers (whose bodies then
  /// work on ctx too), i.e. the code generated without non_volatile_as_local. For hand-written asm libraries
  /// with their own register conventions (banjo-nx: the WMV video decoder). A localized caller never reads
  /// ctx's non-volatiles and an excluded function preserves them as the guest ABI says, so the two mix.
  std::vector<std::pair<uint32_t, uint32_t>> nonVolatileLocalExclude;
  /// non_volatile_as_local_keep = ["v64-v77", "v92", "r30", ...]: non-volatile registers that stay in ctx in
  /// every function, for a game whose code uses them outside the ABI: as global registers set once and read
  /// everywhere (banjo-nx: VMX128 constants in v64-v77), or as implicit arguments a caller sets for a callee
  /// (banjo-nx: v92). A save/restore helper whose range covers one of them is called, not elided.
  uint32_t nonVolatileKeepR = 0;     ///< bit i: ri
  uint32_t nonVolatileKeepF = 0;     ///< bit i: fi
  uint64_t nonVolatileKeepV[2] = {};  ///< bit i of [i / 64]: vi
  bool nonVolatileKept(char kind, size_t index) const {
    if (kind == 'r') return index < 32 && (nonVolatileKeepR >> index & 1);
    if (kind == 'f') return index < 32 && (nonVolatileKeepF >> index & 1);
    return index < 128 && (nonVolatileKeepV[index / 64] >> (index % 64) & 1);
  }
  bool nonVolatileLocalExcluded(uint32_t address) const {
    for (const auto& [first, end] : nonVolatileLocalExclude)
      if (address >= first && address < end)
        return true;
    return false;
  }
  bool generateExceptionHandlers = false;  ///< Generate SEH exception handler wrappers

  /// Arguments in registers (codegen/args_in_registers.h): direct calls between recompiled functions pass
  /// r3-r10, f1-f13, r0/r11/r12/f0 and lr as C++ arguments and return r3 as the C++ return value. The
  /// functions named in `argsInRegistersExcludeFile` (hooked ones) keep the old ABI.
  bool argsInRegisters = false;
  std::string argsInRegistersExcludeFile;  ///< one emitted function name (sub_82XXXXXX) per line
  bool argsInRegistersInstrument = false;  ///< ME_ARGS_COUNT entries (validation builds)
  bool argsInRegistersPoison = false;      ///< ME_ARGS_POISON before fast calls (validation builds)

  // === Analysis tuning (optional) ===
  uint32_t maxJumpExtension = 65536;  ///< Max bytes to extend function for jump table targets
  uint32_t dataRegionThreshold = 16;  ///< Consecutive invalid instructions to mark as data region
  uint32_t largeFunctionThreshold = 1048576;  ///< 1MB - warn if function exceeds this size

  // Optional override for DLL module flag. If unset, the orchestrator infers
  // from the module's position in the manifest (entrypoint = false, modules = true).
  std::optional<bool> isDll;

  // === Manual overrides ===
  std::unordered_map<uint32_t, FunctionConfig> functions;  ///< Function/chunk configuration
  std::unordered_map<uint32_t, JumpTable> switchTables;
  std::unordered_map<uint32_t, MidAsmHook> midAsmHooks;
  uint32_t longJmpAddress = 0;
  uint32_t setJmpAddress = 0;

  // === rexcrt: CRT function address overrides ===
  // Maps function name -> guest address (e.g. "CreateFileA" -> 0x8248B780)
  // Parsed from [rexcrt] TOML table. Codegen generates rexcrt_<Name> entries.
  std::unordered_map<std::string, uint32_t> rexcrtFunctions;

  // === User hints (merged with analysis results in AnalysisState) ===
  std::unordered_map<uint32_t, uint32_t> invalidInstructionHints;  ///< addr -> size
  std::unordered_set<uint32_t>
      knownIndirectCallHints;  ///< bctr addresses that are vtable/computed calls
  std::vector<uint32_t> exceptionHandlerFuncHints;  ///< Additional exception handler addresses

  /**
   * Load configuration from a TOML file.
   *
   * Supports an optional `includes` array for layered config. Paths in
   * `includes` resolve relative to the including file's directory.  Merge
   * semantics: scalars last-wins, keyed tables additive (same key = last
   * wins), arrays-of-tables deduplicated by primary key, sets additive.
   *
   * @param configFilePath Path to the TOML config file
   * @return true on success, false on error (parse failure, circular
   *         include, depth exceeded)
   */
  bool Load(const std::string_view& configFilePath);

  /**
   * Load configuration from an in-memory TOML table (e.g. an inline binary
   * entry inside a manifest). Includes referenced from the table resolve
   * relative to `base_dir`. Same merge semantics as Load().
   */
  bool LoadFromTable(const toml::table& tbl, const std::filesystem::path& base_dir);

  /// Validation result containing warnings and errors.
  struct ValidationResult {
    bool valid = true;                  ///< true if no errors (warnings OK)
    std::vector<std::string> warnings;  ///< Non-fatal issues
    std::vector<std::string> errors;    ///< Fatal issues that block codegen

    explicit operator bool() const { return valid; }
  };

  /**
   * Validate the loaded configuration.
   * Checks address alignment, required fields, and sanity constraints.
   * @return ValidationResult with warnings and errors
   */
  ValidationResult Validate() const;
};

}  // namespace rex::codegen
