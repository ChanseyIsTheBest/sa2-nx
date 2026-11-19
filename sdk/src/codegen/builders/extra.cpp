/**
 * @file        rexcodegen/builders/extra.cpp
 * @brief       Instructions the generator had no builder for
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * renut-ultimate (2026-11-16, port 144; tw2de_nx's builders/extra.cpp of 2026-11-01, as it was, in the table only
 * with the codegen option extra_instructions, off by default): the instructions Xenia's PowerPC instruction tests
 * (src/xenia/cpu/ppc/testing) use and the generator wrote as REX_UNIMPLEMENTED (a trap): vaddcuw vsubcuw vavguw vmaxuw
 * vrlb vpkpx, lmw ldbrx stdbrx rotld, mcrxr, mfvscr mtvscr, and the OE forms (addo ... divduo). (The other missing ones
 * are table entries for builders that exist: see instruction_dispatch.cpp.)
 */

#include "builder_context.h"
#include "helpers.h"

namespace rex::codegen {

namespace {
std::string XFormEA(BuilderContext& ctx, uint32_t ra, uint32_t rb) {
  if (ra == 0)
    return fmt::format("{}.u32", ctx.r(rb));
  return fmt::format("{}.u32 + {}.u32", ctx.r(ra), ctx.r(rb));
}
}  // namespace

// renut-ultimate (2026-11-19, port 187; found by sasrt-nx's sync): port 144's builders are in their own namespace,
// so a port that has its own build_ldbrx (or any other of these) in rex::codegen still links; the dispatch table
// names them as extra_insn::build_*.
namespace extra_insn {

// vaddcuw: each word, the carry out of a + b (unsigned): 1 when b > ~a.
bool build_vaddcuw(BuilderContext& ctx) {
  ctx.println("\t{{");
  ctx.println("\t\tconst simde__m128i na = simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
              "simde_mm_set1_epi32(-1));",
              ctx.v(ctx.insn.operands[1]));
  ctx.println("\t\tconst simde__m128i b = simde_mm_load_si128((simde__m128i*){}.u32);", ctx.v(ctx.insn.operands[2]));
  ctx.println("\t\tconst simde__m128i ge = simde_mm_cmpeq_epi32(simde_mm_max_epu32(na, b), na);  // ~a >= b");
  ctx.println("\t\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_andnot_si128(ge, simde_mm_set1_epi32(1)));",
              ctx.v(ctx.insn.operands[0]));
  ctx.println("\t}}");
  return true;
}

// vsubcuw: each word, the carry out of a - b (unsigned, a + ~b + 1): 1 when a >= b.
bool build_vsubcuw(BuilderContext& ctx) {
  ctx.println("\t{{");
  ctx.println("\t\tconst simde__m128i a = simde_mm_load_si128((simde__m128i*){}.u32);", ctx.v(ctx.insn.operands[1]));
  ctx.println("\t\tconst simde__m128i b = simde_mm_load_si128((simde__m128i*){}.u32);", ctx.v(ctx.insn.operands[2]));
  ctx.println("\t\tconst simde__m128i ge = simde_mm_cmpeq_epi32(simde_mm_max_epu32(a, b), a);  // a >= b");
  ctx.println("\t\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_and_si128(ge, simde_mm_set1_epi32(1)));",
              ctx.v(ctx.insn.operands[0]));
  ctx.println("\t}}");
  return true;
}

// vavguw: each word, (a + b + 1) >> 1 unsigned, without the 33rd bit: (a | b) - ((a ^ b) >> 1).
bool build_vavguw(BuilderContext& ctx) {
  ctx.println("\t{{");
  ctx.println("\t\tconst simde__m128i a = simde_mm_load_si128((simde__m128i*){}.u32);", ctx.v(ctx.insn.operands[1]));
  ctx.println("\t\tconst simde__m128i b = simde_mm_load_si128((simde__m128i*){}.u32);", ctx.v(ctx.insn.operands[2]));
  ctx.println("\t\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_sub_epi32(simde_mm_or_si128(a, b), "
              "simde_mm_srli_epi32(simde_mm_xor_si128(a, b), 1)));",
              ctx.v(ctx.insn.operands[0]));
  ctx.println("\t}}");
  return true;
}

bool build_vmaxuw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epu32", "u32");
  return true;
}

// vrlb: each byte rotated left by the low 3 bits of the same byte of vB.
bool build_vrlb(BuilderContext& ctx) {
  ctx.println("\t{{");
  ctx.println("\t\tPPCVRegister a = {}, b = {};", ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  ctx.println("\t\tfor (int i = 0; i < 16; ++i) {{");
  ctx.println("\t\t\tconst unsigned s = b.u8[i] & 7;");
  ctx.println("\t\t\t{}.u8[i] = uint8_t((a.u8[i] << s) | (a.u8[i] >> ((8 - s) & 7)));", ctx.v(ctx.insn.operands[0]));
  ctx.println("\t\t}}");
  ctx.println("\t}}");
  return true;
}

// vpkpx: the 8 words of vA:vB packed into 1:5:5:5 halfwords (word bits 7, 8..12, 16..20, 24..28). PowerPC
// halfword i is host u16[7 - i] and word j host u32[3 - j]: vA's words go to host u16[4..7], vB's to u16[0..3].
bool build_vpkpx(BuilderContext& ctx) {
  ctx.println("\t{{");
  ctx.println("\t\tPPCVRegister a = {}, b = {};", ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  ctx.println("\t\tauto px = [](uint32_t w) {{ return uint16_t(((w >> 9) & 0xFC00) | ((w >> 6) & 0x03E0) | "
              "((w >> 3) & 0x001F)); }};");
  ctx.println("\t\tfor (int k = 0; k < 4; ++k) {{");
  ctx.println("\t\t\t{0}.u16[4 + k] = px(a.u32[k]);", ctx.v(ctx.insn.operands[0]));
  ctx.println("\t\t\t{0}.u16[k] = px(b.u32[k]);", ctx.v(ctx.insn.operands[0]));
  ctx.println("\t\t}}");
  ctx.println("\t}}");
  return true;
}

// lmw: rD..r31 from EA, EA + 4, ...
bool build_lmw(BuilderContext& ctx) {
  const uint32_t rD = ctx.insn.operands[0];
  const int32_t offset = static_cast<int32_t>(ctx.insn.operands[1]);
  const uint32_t rA = ctx.insn.operands[2];
  if (rA != 0)
    ctx.println("\t{} = {}.u32 + {};", ctx.ea(), ctx.r(rA), offset);
  else
    ctx.println("\t{} = {};", ctx.ea(), offset);
  for (uint32_t i = rD; i <= 31; ++i)
    ctx.println("\t{}.u64 = REX_LOAD_U32({} + {});", ctx.r(i), ctx.ea(), (i - rD) * 4);
  return true;
}

bool build_ldbrx(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = __builtin_bswap64(REX_LOAD_U64({}));", ctx.r(ctx.insn.operands[0]),
              XFormEA(ctx, ctx.insn.operands[1], ctx.insn.operands[2]));
  return true;
}

bool build_stdbrx(BuilderContext& ctx) {
  ctx.println("\tREX_STORE_U64({}, __builtin_bswap64({}.u64));",
              XFormEA(ctx, ctx.insn.operands[1], ctx.insn.operands[2]), ctx.r(ctx.insn.operands[0]));
  return true;
}

// rotld rA, rS, rB: rldcl with mb = 0.
bool build_rotld(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}.u8 & 0x3F);", ctx.r(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), ctx.r(ctx.insn.operands[2]));
  emitRecordFormCompare(ctx);
  return true;
}

// mcrxr crfD: XER's SO OV CA to the field (its 4th bit 0), and cleared in XER.
bool build_mcrxr(BuilderContext& ctx) {
  const auto cr = ctx.cr(ctx.insn.operands[0]);
  const auto xer = ctx.xer();
  ctx.println("\t{}.set_raw((uint32_t({}.so) << 3) | (uint32_t({}.ov) << 2) | (uint32_t({}.ca) << 1));", cr, xer, xer,
              xer);
  ctx.println("\t{0}.so = 0; {0}.ov = 0; {0}.ca = 0;", xer);
  return true;
}

// mfvscr / mtvscr: VSCR (NJ, bit 15 of the last word, and SAT, bit 31) in the last PowerPC word, host u32[0].
bool build_mfvscr(BuilderContext& ctx) {
  const auto vD = ctx.v(ctx.insn.operands[0]);
  ctx.println("\t{0}.u32[3] = 0; {0}.u32[2] = 0; {0}.u32[1] = 0;", vD);
  ctx.println("\t{}.u32[0] = (uint32_t(ctx.vscr_nj) << 16) | uint32_t(ctx.vscr_sat);", vD);
  return true;
}

bool build_mtvscr(BuilderContext& ctx) {
  const auto vB = ctx.v(ctx.insn.operands[0]);
  ctx.println("\tctx.vscr_sat = {}.u32[0] & 1;", vB);
  ctx.println("\tctx.vscr_nj = ({}.u32[0] >> 16) & 1;", vB);
  return true;
}


//=============================================================================
// tw2de_nx (2026-11-01): the OE forms (addo, subfo, mullwo, divwo, ...): the base instruction, then XER[OV] (and SO,
// which keeps it) from its operands, judged on the low 32 bits as the 360's 32-bit mode does (the 64-bit ones on 64).
// The record forms' CR0 comes after, so that its SO is the new one. They had no table entry (REX_UNIMPLEMENTED).
//=============================================================================

}  // namespace extra_insn

// The base instructions the OE forms build on (rex::codegen's).
bool build_add(BuilderContext& ctx);
bool build_addc(BuilderContext& ctx);
bool build_adde(BuilderContext& ctx);
bool build_addme(BuilderContext& ctx);
bool build_addze(BuilderContext& ctx);
bool build_subf(BuilderContext& ctx);
bool build_subfc(BuilderContext& ctx);
bool build_subfe(BuilderContext& ctx);
bool build_subfme(BuilderContext& ctx);
bool build_subfze(BuilderContext& ctx);
bool build_neg(BuilderContext& ctx);
bool build_mullw(BuilderContext& ctx);
bool build_mulld(BuilderContext& ctx);
bool build_divw(BuilderContext& ctx);
bool build_divwu(BuilderContext& ctx);
bool build_divd(BuilderContext& ctx);
bool build_divdu(BuilderContext& ctx);

namespace extra_insn {

namespace {
// ov: an expression of oa / ob (the operands' low words, or oA / oB, 64-bit), oc (the carry in) and r / R (the
// result's low word / doubleword). b: "" when the instruction has no rB (the expression's ob is then given).
bool EmitOE(BuilderContext& ctx, bool (*base)(BuilderContext&), const char* ov, bool has_b, const char* ob_none) {
  const auto xer = ctx.xer();
  const auto rD = ctx.r(ctx.insn.operands[0]);
  ctx.println("\t{{");
  ctx.println("\t\tconst uint32_t oa = {0}.u32; const uint64_t oA = {0}.u64;", ctx.r(ctx.insn.operands[1]));
  if (has_b)
    ctx.println("\t\tconst uint32_t ob = {0}.u32; const uint64_t oB = {0}.u64;", ctx.r(ctx.insn.operands[2]));
  else
    ctx.println("\t\tconst uint32_t ob = {}; const uint64_t oB = 0;", ob_none);
  ctx.println("\t\tconst uint32_t oc = {}.ca; (void)oc; (void)oA; (void)oB;", xer);
  // The base instruction (its record form's CR0 is written again below, with the new SO).
  const bool record = isRecordForm(ctx.insn);
  const bool ok = base(ctx);
  ctx.println("\t\tconst uint32_t r = {0}.u32; const uint64_t R = {0}.u64; (void)r; (void)R;", rD);
  ctx.println("\t\t{}.ov = ({}) ? 1 : 0;", xer, ov);
  ctx.println("\t\t{0}.so |= {0}.ov;", xer);
  ctx.println("\t}}");
  if (record)
    ctx.println("\t{}.compare<int32_t>({}.s32, 0, {});", ctx.cr(0), rD, xer);
  return ok;
}

constexpr const char* kAddOV = "(((oa ^ r) & (ob ^ r)) >> 31) & 1";
constexpr const char* kSubfOV = "(((~oa ^ r) & (ob ^ r)) >> 31) & 1";
}  // namespace

bool build_addo(BuilderContext& ctx) { return EmitOE(ctx, build_add, kAddOV, true, ""); }
bool build_addco(BuilderContext& ctx) { return EmitOE(ctx, build_addc, kAddOV, true, ""); }
bool build_addeo(BuilderContext& ctx) { return EmitOE(ctx, build_adde, kAddOV, true, ""); }
bool build_addmeo(BuilderContext& ctx) { return EmitOE(ctx, build_addme, kAddOV, false, "0xFFFFFFFFu"); }
bool build_addzeo(BuilderContext& ctx) { return EmitOE(ctx, build_addze, kAddOV, false, "0u"); }
bool build_subfo(BuilderContext& ctx) { return EmitOE(ctx, build_subf, kSubfOV, true, ""); }
bool build_subfco(BuilderContext& ctx) { return EmitOE(ctx, build_subfc, kSubfOV, true, ""); }
bool build_subfeo(BuilderContext& ctx) { return EmitOE(ctx, build_subfe, kSubfOV, true, ""); }
bool build_subfmeo(BuilderContext& ctx) { return EmitOE(ctx, build_subfme, kSubfOV, false, "0xFFFFFFFFu"); }
bool build_subfzeo(BuilderContext& ctx) { return EmitOE(ctx, build_subfze, kSubfOV, false, "0u"); }
bool build_nego(BuilderContext& ctx) { return EmitOE(ctx, build_neg, "oa == 0x80000000u", false, "0u"); }
bool build_mullwo(BuilderContext& ctx) {
  return EmitOE(ctx, build_mullw,
                "int64_t(int32_t(oa)) * int64_t(int32_t(ob)) != int64_t(int32_t(uint32_t(int64_t(int32_t(oa)) * "
                "int64_t(int32_t(ob)))))",
                true, "");
}
bool build_mulldo(BuilderContext& ctx) {
  return EmitOE(ctx, build_mulld,
                "__int128(int64_t(oA)) * __int128(int64_t(oB)) != __int128(int64_t(uint64_t(oA) * uint64_t(oB)))",
                true, "");
}
bool build_divwo(BuilderContext& ctx) {
  return EmitOE(ctx, build_divw, "ob == 0 || (oa == 0x80000000u && ob == 0xFFFFFFFFu)", true, "");
}
bool build_divwuo(BuilderContext& ctx) { return EmitOE(ctx, build_divwu, "ob == 0", true, ""); }
bool build_divdo(BuilderContext& ctx) {
  return EmitOE(ctx, build_divd, "oB == 0 || (oA == 0x8000000000000000ull && oB == ~0ull)", true, "");
}
bool build_divduo(BuilderContext& ctx) { return EmitOE(ctx, build_divdu, "oB == 0", true, ""); }

}  // namespace extra_insn

}  // namespace rex::codegen
