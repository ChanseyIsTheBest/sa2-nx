// sa2-nx (2026-10-28): hand-written sub_82833568, the hottest guest function of the game thread in console gameplay
// (8.9 % of its samples, round-6 renut profile): a DXT1 colour block to 16 RGBA float texels. It builds the block's
// four-colour palette from its two RGB565 endpoints (VMX, on the stack below r1: [r1-176, r1-112)), then writes each
// texel's palette entry, by its 2-bit index, to [r3 + 16k]. Made from its localized copy (app/generated/hot/n_82833568.h),
// which stays its reference: re-run tools/hand_natives/make_dxt1_block.py after the copy changes (it checks the loop it
// replaces statement by statement).
//
// Optimisation: the copy's texel loop reloads the entry's four words (byte swaps), converts three to double and back
// (quieting) and the fourth through double, stores four byte-swapped words and marks four cards a texel, 16 times. The
// fast loop converts the four entries once into their stored (big-endian) bytes and copies 16 bytes a texel, marks the
// cards of the 256 bytes once, and sets the registers the copy's last texel leaves.
//
// Audit: it applies when the output [r3, r3+256) neither wraps at 4 GB nor crosses the hosts' physical-range offset
// (one host range) nor overlaps the palette [r1-176, r1-112): then the copy's loop reads the same palette at every
// texel (nothing it stores lands on it), so each texel's 16 bytes are its entry's words as the copy converts them:
// QuietSingleBits of words 0-2, float(double(float)) of word 3, byte-swapped as REX_STORE_U32 stores them. The index
// of texel k is bits 2k..2k+1 of the rotated word (the copy's (r9 << 4) & 0x30 and r9 >>= 2). The registers: r8 =
// the last index * 16, r9 = 0 (sixteen 2-bit shifts of a 32-bit word), r10 = r1 - 176 + r8 (64-bit, as the copy),
// f13 / f12 = words 1 / 2 of the last entry as double (unquieted, as the copy loads them), f11 = word 3 through
// double; ctr, r11, f0, ea and temp are the copy's locals, dead after the loop. The copy's disableFlushMode at every
// texel runs once. The cards are the pages of the 64 stores' first bytes, zeroed after the data as the copy zeroes each
// after its store. Otherwise the copy's loop runs.
//
// Unguarded like its copy was; in verify mode (sa2_cpu_verify) the fast loop checks itself: every block it takes is
// written by both loops from the same inputs (the output bytes and registers saved and put back), the copy's result
// kept, and any difference counted in the [verify] line and logged as "[hot] DIFFERENCE".
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include <rex/logging.h>

#include "../sa2_enhancements.h"
#include "../sa2_hot_common.h"


namespace sa2::hot::n_dxt1_block {

// Verify mode: the block's 256 output bytes and the registers either loop writes, saved to run the copy's loop on the
// same inputs and compare.
struct BlockCheck {
  std::vector<uint8_t> before, fast;
  uint64_t in[10], out[10];
};
inline constexpr int kBlockRegs = 6;  // compared: the first 6 (ctr, r11, f0 and ea are the copy's dead locals)
// (Named one by one: an array of their addresses would make them address-taken, kept in memory in the fast loop too.)
#define SA2_BLOCK_REGS(a, op)                                                                                  \
  do {                                                                                                         \
    (a)[0] op l_r8.u64; (a)[1] op l_r9.u64; (a)[2] op l_r10.u64; (a)[3] op l_f11.u64; (a)[4] op l_f12.u64;     \
    (a)[5] op l_f13.u64; (a)[6] op ctr.u64; (a)[7] op r11.u64; (a)[8] op f0.u64; (a)[9] op uint64_t(ea);       \
  } while (0)
#define SA2_BLOCK_REGS_BACK(a)                                                                                 \
  do {                                                                                                         \
    l_r8.u64 = (a)[0]; l_r9.u64 = (a)[1]; l_r10.u64 = (a)[2]; l_f11.u64 = (a)[3]; l_f12.u64 = (a)[4];          \
    l_f13.u64 = (a)[5]; ctr.u64 = (a)[6]; r11.u64 = (a)[7]; f0.u64 = (a)[8]; ea = uint32_t((a)[9]);            \
  } while (0)
inline void Save(const uint8_t* base, uint32_t a, uint32_t len, std::vector<uint8_t>& out) {
  out.resize(len);
  for (uint32_t k = 0; k < len; ++k) out[k] = *Raw(base, a + k);
}
inline void Restore(uint8_t* base, uint32_t a, const std::vector<uint8_t>& in) {
  for (uint32_t k = 0; k < in.size(); ++k) *Raw(base, a + k) = in[k];
}


inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f6 = ctx.f6;
  PPCRegister l_f7 = ctx.f7;
  PPCRegister l_f8 = ctx.f8;
  PPCRegister l_f9 = ctx.f9;
  PPCRegister l_f10 = ctx.f10;
  PPCRegister l_f11 = ctx.f11;
  PPCRegister l_f12 = ctx.f12;
  PPCRegister l_f13 = ctx.f13;
  PPCRegister l_r1 = ctx.r1;
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r4 = ctx.r4;
  PPCRegister l_r5 = ctx.r5;
  PPCRegister l_r6 = ctx.r6;
  PPCRegister l_r7 = ctx.r7;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
  PPCVRegister l_v0 = ctx.v0;
  PPCVRegister l_v4 = ctx.v4;
  PPCVRegister l_v5 = ctx.v5;
  PPCVRegister l_v6 = ctx.v6;
  PPCVRegister l_v7 = ctx.v7;
  PPCVRegister l_v12 = ctx.v12;
  PPCVRegister l_v13 = ctx.v13;
#define SA2_SYNC_OUT() do { ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; ctx.v0 = l_v0; ctx.v4 = l_v4; ctx.v5 = l_v5; ctx.v6 = l_v6; ctx.v7 = l_v7; ctx.v12 = l_v12; ctx.v13 = l_v13; } while (0)
#define SA2_SYNC_IN() do { l_f6 = ctx.f6; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; l_v0 = ctx.v0; l_v4 = ctx.v4; l_v5 = ctx.v5; l_v6 = ctx.v6; l_v7 = ctx.v7; l_v12 = ctx.v12; l_v13 = ctx.v13; } while (0)
	REX_FUNC_PROLOGUE();
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r20{};
	PPCRegister r21{};
	PPCRegister r22{};
	PPCRegister r23{};
	PPCRegister r24{};
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f0{};
	PPCVRegister v60{};
	PPCVRegister v61{};
	PPCVRegister v62{};
	PPCVRegister v63{};
	PPCRegister temp{};
	PPCRegister fb0{};
	PPCRegister fb12{};
	PPCRegister fb13{};
	uint32_t ea{};
	BlockCheck* block_check = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	l_r8.s64 = -2113536000;
	l_r9.u64 = REX_LOAD_D_U16(l_r4.u32, 2);
	l_r10.s64 = -2113536000;
	r11.u64 = REX_LOAD_D_U16(l_r4.u32, 0);
	l_r6.u64 = (l_r9.u32 >> 11);
	l_r7.u64 = (r11.u32 >> 11);
	r31.u64 = l_r9.u32 & 0x1F;
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_RO_U32(0x8205F6C4, 0x3C820821);
	l_f12.f64 = double(temp.f32);
	l_r8.u64 = l_r6.u64;
	temp.u32 = REX_RO_U32(0x8205F6C0, 0x3D042108);
	f0.f64 = double(temp.f32);
	l_r10.u64 = ((r11.u32 >> 5) & 0x3Fu);
	REX_STORE_D_U64(l_r1.u32, -224, l_r8.u64);
	l_f11.u64 = REX_LOAD_D_U64(l_r1.u32, -224);
	REX_STORE_D_U64(l_r1.u32, -224, l_r10.u64);
	l_r10.u64 = r11.u32 & 0x1F;
	REX_STORE_D_U64(l_r1.u32, -192, l_r7.u64);
	l_f8.u64 = REX_LOAD_D_U64(l_r1.u32, -192);
	REX_STORE_D_U64(l_r1.u32, -192, l_r10.u64);
	l_f7.u64 = REX_LOAD_D_U64(l_r1.u32, -192);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, -224);
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -208, r31.u64));
	l_f9.u64 = REX_LOAD_D_U64(l_r1.u32, -208);
	l_r8.u64 = l_r7.u64;
	l_r5.u64 = ((l_r9.u32 >> 5) & 0x3Fu);
	l_f11.f64 = double(l_f11.s64);
	l_r8.s64 = -2113929216;
	l_f11.f64 = double(float(l_f11.f64));
	l_r6.u64 = l_r5.u64;
	l_f8.f64 = double(l_f8.s64);
	l_f7.f64 = double(l_f7.s64);
	l_r10.s64 = l_r1.s64 + -176;
	l_f6.f64 = double(l_f13.s64);
	REX_STORE_D_U64(l_r1.u32, -216, l_r6.u64);
	l_f10.u64 = REX_LOAD_D_U64(l_r1.u32, -216);
	l_r5.u64 = r31.u64;
	fb13.u32 = REX_RO_CFG_U32(0x820018B8, 0x3F800000u, REX_LOAD_D_U32(l_r8.u32, 6328));
	l_f13.f64 = rex::ppc::SingleToDoublePure(fb13.u32);
	l_r8.s64 = l_r1.s64 + -160;
	temp.u32 = rex::ppc::QuietSingleBits(fb13.u32);
	REX_STORE_D_U32(l_r1.u32, -164, temp.u32);
	cr6.compare<uint32_t>(r11.u32, l_r9.u32, xer);
	temp.u32 = rex::ppc::QuietSingleBits(fb13.u32);
	REX_STORE_D_U32(l_r1.u32, -148, temp.u32);
	l_f13.f64 = double(l_f10.s64);
	l_f10.f64 = double(l_f9.s64);
	l_f9.f64 = double(float(l_f7.f64));
	l_f7.f64 = double(float(l_f6.f64));
	l_f8.f64 = double(float(l_f8.f64));
	l_f13.f64 = double(float(l_f13.f64));
	l_f11.f64 = REX_FMULS(l_f11.f64, f0.f64);
	temp.f32 = float(l_f11.f64);
	REX_STORE_D_U32(l_r1.u32, -160, temp.u32);
	l_f10.f64 = double(float(l_f10.f64));
	l_f9.f64 = REX_FMULS(l_f9.f64, f0.f64);
	temp.f32 = float(l_f9.f64);
	REX_STORE_D_U32(l_r1.u32, -168, temp.u32);
	l_f9.f64 = REX_FMULS(l_f7.f64, l_f12.f64);
	temp.f32 = float(l_f9.f64);
	REX_STORE_D_U32(l_r1.u32, -172, temp.u32);
	l_f9.f64 = REX_FMULS(l_f8.f64, f0.f64);
	temp.f32 = float(l_f9.f64);
	REX_STORE_D_U32(l_r1.u32, -176, temp.u32);
	l_f13.f64 = REX_FMULS(l_f13.f64, l_f12.f64);
	temp.f32 = float(l_f13.f64);
	REX_STORE_D_U32(l_r1.u32, -156, temp.u32);
	f0.f64 = REX_FMULS(l_f10.f64, f0.f64);
	temp.f32 = float(f0.f64);
	REX_STORE_D_U32(l_r1.u32, -152, temp.u32);
	if (cr6.gt) goto loc_8283370C;
	r11.s64 = l_r1.s64 + -161;
	ea = (l_r10.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v62.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	l_r7.s64 = l_r1.s64 + -176;
	ea = (l_r8.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v60.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	l_r9.s64 = l_r1.s64 + -145;
	l_r6.s64 = l_r1.s64 + -160;
	l_r10.s64 = l_r1.s64 + -192;
	ea = (r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	r11.s64 = -2113929216;
	temp.u32 = l_r7.u32;
	simde_mm_store_si128((simde__m128i*)l_v0.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(temp.u32 & 0xF) * 16]));
	l_r8.s64 = l_r1.s64 + -144;
	ea = (l_r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v61.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	l_r9.s64 = l_r1.s64 + -144;
	temp.u32 = l_r6.u32;
	simde_mm_store_si128((simde__m128i*)l_v7.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(temp.u32 & 0xF) * 16]));
	simde_mm_store_si128((simde__m128i*)l_v0.u8, rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)v62.u8), simde_mm_load_si128((simde__m128i*)v63.u8), simde_mm_load_si128((simde__m128i*)l_v0.u8)));
	l_r7.s64 = l_r1.s64 + -144;
	fb0.u32 = REX_RO_CFG_U32(0x8200154C, 0x3F000000u, REX_LOAD_D_U32(r11.u32, 5452));
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	simde_mm_store_si128((simde__m128i*)v63.u8, rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)v60.u8), simde_mm_load_si128((simde__m128i*)v61.u8), simde_mm_load_si128((simde__m128i*)l_v7.u8)));
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -188, temp.u32);
	r11.s64 = 4;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -180, temp.u32);
	l_r6.s64 = l_r1.s64 + -144;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -184, temp.u32);
	l_r5.s64 = 8;
	ctx.fpscr.enableFlushModeUnconditional();
	simde_mm_store_ps(l_v13.f32, simde_mm_sub_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(l_v0.f32)));
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -192, temp.u32);
	temp.u32 = l_r9.u32;
	simde_mm_store_si128((simde__m128i*)l_v7.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableR[(temp.u32 & 0xF) * 16]));
	l_r9.s64 = 12;
	r31.s64 = -2113929216;
	ea = (l_r10.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v12.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	simde_mm_store_ps(l_v0.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(l_v12.f32)), simde_mm_load_ps(l_v0.f32)));
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_VPERM1(simde_mm_load_si128((simde__m128i*)l_v0.u8), simde_mm_load_si128((simde__m128i*)l_v7.u8)));
	l_r10.s64 = l_r1.s64 + -144;
	ea = (l_r8.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (l_r7.u32 + r11.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (l_r6.u32 + l_r5.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (l_r10.u32 + l_r9.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	fb0.u32 = REX_RO_CFG_U32(0x82001630, 0x0u, REX_LOAD_D_U32(r31.u32, 5680));
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -128, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -124, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -120, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -116, temp.u32);
	goto loc_82833828;
loc_8283370C:
	r11.s64 = -2113536000;
	ea = (l_r10.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	l_r9.s64 = l_r1.s64 + -161;
	ea = (l_r8.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v61.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	l_r6.s64 = l_r1.s64 + -176;
	l_r7.s64 = l_r1.s64 + -145;
	l_r5.s64 = l_r1.s64 + -192;
	fb0.u32 = REX_RO_CFG_U32(0x8205F6D0, 0x3EAAAAABu, REX_LOAD_D_U32(r11.u32, -2352));
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	r11.s64 = l_r1.s64 + -160;
	ea = (l_r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v62.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	r31.s64 = l_r1.s64 + -144;
	temp.u32 = l_r6.u32;
	simde_mm_store_si128((simde__m128i*)l_v0.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(temp.u32 & 0xF) * 16]));
	r30.s64 = l_r1.s64 + -144;
	ea = (l_r7.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v60.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	l_r9.s64 = 4;
	simde_mm_store_si128((simde__m128i*)l_v0.u8, rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)v63.u8), simde_mm_load_si128((simde__m128i*)v62.u8), simde_mm_load_si128((simde__m128i*)l_v0.u8)));
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -180, temp.u32);
	temp.u32 = r11.u32;
	simde_mm_store_si128((simde__m128i*)l_v7.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(temp.u32 & 0xF) * 16]));
	l_r8.s64 = l_r1.s64 + -144;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -184, temp.u32);
	l_r10.s64 = 8;
	simde_mm_store_si128((simde__m128i*)v63.u8, rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)v61.u8), simde_mm_load_si128((simde__m128i*)v60.u8), simde_mm_load_si128((simde__m128i*)l_v7.u8)));
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -188, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -192, temp.u32);
	l_r7.s64 = l_r1.s64 + -144;
	ea = (l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v12.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	r11.s64 = 12;
	temp.u32 = r31.u32;
	simde_mm_store_si128((simde__m128i*)l_v7.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableR[(temp.u32 & 0xF) * 16]));
	l_r6.s64 = l_r1.s64 + -144;
	ctx.fpscr.enableFlushMode();
	simde_mm_store_ps(l_v13.f32, simde_mm_sub_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(l_v0.f32)));
	r27.s64 = l_r1.s64 + -145;
	r26.s64 = l_r1.s64 + -160;
	r28.s64 = l_r1.s64 + -192;
	r31.s64 = l_r1.s64 + -161;
	r29.s64 = l_r1.s64 + -176;
	r25.s64 = -2113536000;
	r24.s64 = l_r1.s64 + -176;
	r23.s64 = l_r1.s64 + -160;
	r22.s64 = l_r1.s64 + -128;
	r21.s64 = l_r1.s64 + -128;
	r20.s64 = l_r1.s64 + -128;
	temp.u32 = r24.u32;
	simde_mm_store_si128((simde__m128i*)l_v6.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(temp.u32 & 0xF) * 16]));
	l_r5.s64 = l_r1.s64 + -128;
	simde_mm_store_ps(l_v0.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(l_v12.f32)), simde_mm_load_ps(l_v0.f32)));
	temp.u32 = r23.u32;
	simde_mm_store_si128((simde__m128i*)l_v5.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(temp.u32 & 0xF) * 16]));
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_VPERM1(simde_mm_load_si128((simde__m128i*)l_v0.u8), simde_mm_load_si128((simde__m128i*)l_v7.u8)));
	temp.u32 = r22.u32;
	simde_mm_store_si128((simde__m128i*)l_v4.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableR[(temp.u32 & 0xF) * 16]));
	r24.s64 = l_r1.s64 + -128;
	ea = (r30.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (l_r8.u32 + l_r9.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (l_r7.u32 + l_r10.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (l_r6.u32 + r11.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (r27.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v61.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	ea = (r26.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v60.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	fb0.u32 = REX_RO_CFG_U32(0x82067048, 0x3F2AAAABu, REX_LOAD_D_U32(r25.u32, 28744));
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ea = (r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	ea = (r29.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v62.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	simde_mm_store_si128((simde__m128i*)l_v0.u8, rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)v62.u8), simde_mm_load_si128((simde__m128i*)v63.u8), simde_mm_load_si128((simde__m128i*)l_v6.u8)));
	simde_mm_store_si128((simde__m128i*)v61.u8, rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)v60.u8), simde_mm_load_si128((simde__m128i*)v61.u8), simde_mm_load_si128((simde__m128i*)l_v5.u8)));
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -180, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -184, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -188, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, -192, temp.u32);
	simde_mm_store_ps(l_v13.f32, simde_mm_sub_ps(simde_mm_load_ps(v61.f32), simde_mm_load_ps(l_v0.f32)));
	ea = (r28.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v12.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	simde_mm_store_ps(l_v0.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(l_v12.f32)), simde_mm_load_ps(l_v0.f32)));
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_VPERM1(simde_mm_load_si128((simde__m128i*)l_v0.u8), simde_mm_load_si128((simde__m128i*)l_v4.u8)));
	ea = (r21.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (r20.u32 + l_r9.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (l_r5.u32 + l_r10.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
	ea = (r24.u32 + r11.u32) & ~0x3;
	REX_STORE_U32_C(ea, v63.u32[3 - ((ea & 0xF) >> 2)]);
loc_82833828:
	l_r10.s64 = 16;
	l_r9.u64 = REX_LOAD_D_U32(l_r4.u32, 4);
	r11.s64 = l_r3.s64 + -4;
	l_r9.u64 = __builtin_rotateleft32(l_r9.u32, 16);
	ctr.u64 = l_r10.u64;
	{
		// sa2-nx (2026-10-28): the 16 texels by the fast loop where it applies (the header's conditions), else by the
		// copy's loop below.
		const uint32_t out = l_r3.u32, pal = uint32_t(l_r1.u32 - 176);
		const uint64_t end = uint64_t(out) + 256;
		if (end <= 0x100000000ull && PhysOff(out) == PhysOff(uint32_t(end - 1)) &&
		    (end <= pal || uint64_t(pal) + 64 <= out)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				block_check = new BlockCheck{};
				Save(base, out, 256, block_check->before);
				SA2_BLOCK_REGS(block_check->in, =);
			}
			ctx.fpscr.disableFlushMode();
			// The four palette entries as the copy's loop stores them (words 0-2 quieted, word 3 through double),
			// big-endian: a texel is then one 16-byte copy.
			uint32_t entry[4][4];
			for (uint32_t e = 0; e < 4; ++e) {
				const uint32_t a = pal + 16 * e;
				entry[e][0] = __builtin_bswap32(rex::ppc::QuietSingleBits(REX_LOAD_D_U32(a, 0)));
				entry[e][1] = __builtin_bswap32(rex::ppc::QuietSingleBits(REX_LOAD_D_U32(a, 4)));
				entry[e][2] = __builtin_bswap32(rex::ppc::QuietSingleBits(REX_LOAD_D_U32(a, 8)));
				temp.f32 = float(double(REX_LOAD_D_F32(a, 12)));
				entry[e][3] = __builtin_bswap32(temp.u32);
			}
			uint8_t* const dst = Raw(base, out);
			uint32_t idx = l_r9.u32;  // two bits a texel, the first in the low bits
			for (int k = 0; k < 16; ++k, idx >>= 2) std::memcpy(dst + 16 * k, entry[idx & 3], 16);
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, out, 253);  // the pages of the 64 stores' first bytes, [out, out + 252]
#endif
			// The registers as the copy's last texel (the 16th) leaves them.
			const uint32_t last = l_r9.u32 >> 30;
			l_r8.u64 = ((uint32_t)(last << 4) & 0x30u);
			l_r9.u64 = 0;  // sixteen 2-bit shifts of a 32-bit word
			l_r10.s64 = l_r1.s64 + -176;
			l_r10.u64 = l_r8.u64 + l_r10.u64;
			fb13.u32 = REX_LOAD_D_U32(l_r10.u32, 4);
			l_f13.f64 = rex::ppc::SingleToDoublePure(fb13.u32);
			fb12.u32 = REX_LOAD_D_U32(l_r10.u32, 8);
			l_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
			l_f11.f64 = double(REX_LOAD_D_F32(l_r10.u32, 12));
			if (!block_check) [[likely]] goto loc_8283383C_done;
			// Verify mode: keep the fast loop's result, put the inputs back, and run the copy's loop to compare.
			SA2_BLOCK_REGS(block_check->out, =);
			Save(base, out, 256, block_check->fast);
			Restore(base, out, block_check->before);
			SA2_BLOCK_REGS_BACK(block_check->in);
		}
	}
loc_8283383C:
	REX_LOOP_BARRIER();
	l_r8.u64 = ((uint32_t)(l_r9.u32 << 4) & 0x30u);
	l_r10.s64 = l_r1.s64 + -176;
	l_r9.u64 = (l_r9.u32 >> 2);
	l_r10.u64 = l_r8.u64 + l_r10.u64;
	fb0.u32 = REX_LOAD_D_U32(l_r10.u32, 0);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	fb13.u32 = REX_LOAD_D_U32(l_r10.u32, 4);
	l_f13.f64 = rex::ppc::SingleToDoublePure(fb13.u32);
	fb12.u32 = REX_LOAD_D_U32(l_r10.u32, 8);
	l_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	ctx.fpscr.disableFlushMode();
	l_f11.f64 = double(REX_LOAD_D_F32(l_r10.u32, 12));
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(r11.u32, 4, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb13.u32);
	REX_STORE_D_U32_C(r11.u32, 8, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_D_U32_C(r11.u32, 12, temp.u32);
	ea = 16 + r11.u32;
	temp.f32 = float(l_f11.f64);
	REX_STORE_U32_C(ea, temp.u32);
	r11.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_8283383C;
	if (block_check) [[unlikely]] {
		uint64_t want[kBlockRegs];
		SA2_BLOCK_REGS(want, =);
		std::vector<uint8_t> now;
		Save(base, l_r3.u32, 256, now);
		const bool same = std::equal(want, want + kBlockRegs, block_check->out) && now == block_check->fast;
		::sa2::enh::VerifyCheck();
		static std::atomic<uint32_t> checks_{0};
		const uint32_t count_ = checks_.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count_ == 1 || count_ == 4096 || count_ == 262144)
			REXLOG_INFO("[hot] sub_82833568 fast texel loop: {} checks against the copy's loop", count_);
		if (!same) {
			::sa2::enh::VerifyDifference();
			REXLOG_ERROR("[hot] DIFFERENCE: sub_82833568's fast texel loop (block to {:08X}) differs from the copy's loop; "
			             "the copy's result stands", l_r3.u32);
		}
		delete block_check;
		block_check = nullptr;
	}
loc_8283383C_done:
	l_r3.s64 = 0;
	{ do { ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r3 = l_r3; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; ctx.v0 = l_v0; ctx.v4 = l_v4; ctx.v5 = l_v5; ctx.v6 = l_v6; ctx.v7 = l_v7; ctx.v12 = l_v12; ctx.v13 = l_v13; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

#undef SA2_BLOCK_REGS
#undef SA2_BLOCK_REGS_BACK

}  // namespace sa2::hot::n_dxt1_block
