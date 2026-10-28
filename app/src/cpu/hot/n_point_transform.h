// sa2-nx (2026-10-28): hand-written sub_82147628, an array of r6 points (12 bytes each, from r4) by the 3x4 matrix at
// r3 to r5: four points a pass, then the rest one by one. Made from its localized copy (app/generated/hot/n_82147628.h),
// which stays its reference: re-run tools/hand_natives/make_point_transform.py after the copy changes (it takes the
// loops' statements from the copy and checks the forms it rewrites).
//
// Optimisation: every store marks a card, a byte store that may alias anything, so the copy reloads the matrix's 12
// floats for every point (48 loads a pass of four). The fast loops load the matrix once and mark the outputs' cards
// once.
//
// Audit: a fast loop applies when its outputs (the copy's [r10+4] onwards: 48 bytes a pass, 12 a point, contiguous)
// neither wrap at 4 GB nor overlap the matrix [r3, r3+48) (which is one host range): then every matrix load of the
// copy reads what the fast loop holds. Each statement is otherwise the copy's in its order, its point loads and output
// stores included (so outputs over the inputs, in place, read what the copy reads), with plain stores; the registers
// each loop leaves are the copy's last pass's or point's. The cards are the pages of the stores' first bytes (stores
// four bytes apart over the whole range: every page from the first to the last), zeroed after the data. Otherwise the
// copy's loop runs.
//
// Unguarded like its copy (bulk); in verify mode (sa2_cpu_verify) each fast loop checks itself against the copy's
// (hot/fast_loop_check.h).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include <rex/logging.h>

#include "../sa2_enhancements.h"
#include "../sa2_hot_common.h"
#include "fast_loop_check.h"


namespace sa2::hot::n_point_transform {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f1 = ctx.f1;
  PPCRegister l_f2 = ctx.f2;
  PPCRegister l_f3 = ctx.f3;
  PPCRegister l_f4 = ctx.f4;
  PPCRegister l_f5 = ctx.f5;
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
#define SA2_SYNC_OUT() do { ctx.f1 = l_f1; ctx.f2 = l_f2; ctx.f3 = l_f3; ctx.f4 = l_f4; ctx.f5 = l_f5; ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f1 = ctx.f1; l_f2 = ctx.f2; l_f3 = ctx.f3; l_f4 = ctx.f4; l_f5 = ctx.f5; l_f6 = ctx.f6; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister f0{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check1 = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check4 = nullptr;  // verify mode only
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -8, f31.u64));
	l_r9.s64 = 0;
	cr6.compare<int32_t>(l_r6.s32, 4, xer);
	if (cr6.lt) goto loc_82147838;
	l_r8.s64 = l_r6.s64 + -3;
	l_r10.s64 = l_r5.s64 + -4;
	r11.s64 = l_r4.s64 + 16;
	l_r7.u64 = l_r5.u64 - l_r4.u64;
	ctx.fpscr.disableFlushMode(); // flush_hoist.py
	{
		// sa2-nx (2026-10-28): sub_82147628 fast points (four a pass) where it applies (the header's conditions), else the copy's loop below.
		// Passes: the do-while runs once, then while r9 + 4k < r8 (unsigned).
		const uint64_t passes = l_r8.u32 > l_r9.u32 ? (uint64_t(l_r8.u32) - l_r9.u32 + 3) / 4 : 1;
		const uint64_t first = uint32_t(l_r10.u32 + 4), bytes = 48 * passes;
		if (::sa2::hot::fast_loop::Flat(first, bytes) && ::sa2::hot::fast_loop::Apart(first, bytes, l_r3.u32, 48) && ::sa2::hot::fast_loop::Flat(l_r3.u32, 48)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check4 = new ::sa2::hot::fast_loop::Check{};
				check4->Add(uint32_t(first), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check4, base);
				check4->in[0] = l_f1.u64;
				check4->in[1] = l_f2.u64;
				check4->in[2] = l_f3.u64;
				check4->in[3] = l_f4.u64;
				check4->in[4] = l_f5.u64;
				check4->in[5] = l_f6.u64;
				check4->in[6] = l_f7.u64;
				check4->in[7] = l_f8.u64;
				check4->in[8] = l_f9.u64;
				check4->in[9] = l_f10.u64;
				check4->in[10] = l_f11.u64;
				check4->in[11] = l_f12.u64;
				check4->in[12] = l_f13.u64;
				check4->in[13] = f0.u64;
				check4->in[14] = l_r9.u64;
				check4->in[15] = l_r10.u64;
				check4->in[16] = r11.u64;
				check4->in[17] = f31.u64;
				check4->in[18] = uint64_t(ea);
				check4->in[19] = temp.u64;
			}
			// The matrix [r3, r3+48), reloaded by the copy for every point (no output lands on it).
			float m[12];
			for (uint32_t k = 0; k < 12; ++k) m[k] = REX_LOAD_D_F32(l_r3.u32, 4 * k);
			do {
				REX_LOOP_BARRIER();
				REX_FLUSH_HOIST_KNOWN_OFF();
				f0.f64 = double(REX_LOAD_D_F32(r11.u32, -12));
				l_f13.f64 = double(m[1]);
				l_f12.f64 = double(REX_LOAD_D_F32(r11.u32, -8));
				l_f11.f64 = REX_FMULS(l_f13.f64, f0.f64);
				l_f10.f64 = double(m[6]);
				l_f9.f64 = double(m[10]);
				l_f8.f64 = REX_FMULS(l_f10.f64, l_f12.f64);
				l_f7.f64 = REX_FMULS(l_f9.f64, l_f12.f64);
				l_f6.f64 = double(REX_LOAD_D_F32(r11.u32, -16));
				l_f5.f64 = double(m[0]);
				l_f4.f64 = double(m[4]);
				l_f3.f64 = double(m[9]);
				l_f2.f64 = double(m[2]);
				l_f1.f64 = double(m[5]);
				l_f13.f64 = double(m[8]);
				l_f11.f64 = double(float(std::fma(l_f5.f64, l_f6.f64, l_f11.f64)));
				l_f10.f64 = double(m[3]);
				l_f9.f64 = double(m[7]);
				l_f8.f64 = double(float(std::fma(l_f4.f64, l_f6.f64, l_f8.f64)));
				l_f5.f64 = double(m[11]);
				l_f4.f64 = double(float(std::fma(l_f3.f64, f0.f64, l_f7.f64)));
				l_f3.f64 = double(float(std::fma(l_f2.f64, l_f12.f64, l_f11.f64)));
				l_f2.f64 = double(float(std::fma(l_f1.f64, f0.f64, l_f8.f64)));
				l_f1.f64 = double(float(std::fma(l_f6.f64, l_f13.f64, l_f4.f64)));
				f0.f64 = REX_FADDS(l_f3.f64, l_f10.f64);
				temp.f32 = float(f0.f64);
				REX_STORE_D_U32(l_r10.u32, 4, temp.u32);
				l_f13.f64 = REX_FADDS(l_f2.f64, l_f9.f64);
				temp.f32 = float(l_f13.f64);
				REX_STORE_D_U32(l_r10.u32, 8, temp.u32);
				l_f12.f64 = REX_FADDS(l_f1.f64, l_f5.f64);
				temp.f32 = float(l_f12.f64);
				REX_STORE_D_U32(l_r10.u32, 12, temp.u32);
				l_f11.f64 = double(m[4]);
				l_f10.f64 = double(m[8]);
				l_f9.f64 = double(REX_LOAD_D_F32(r11.u32, 0));
				l_f8.f64 = double(m[1]);
				l_f7.f64 = double(m[5]);
				l_f6.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
				l_f5.f64 = double(m[10]);
				l_f4.f64 = double(m[2]);
				l_f3.f64 = double(m[6]);
				l_f2.f64 = double(m[9]);
				l_f1.f64 = double(m[3]);
				f0.f64 = double(m[7]);
				l_f13.f64 = double(m[11]);
				l_f12.f64 = double(REX_LOAD_D_F32(r11.u32, -4));
				f31.f64 = double(m[0]);
				f31.f64 = REX_FMULS(f31.f64, l_f12.f64);
				l_f11.f64 = REX_FMULS(l_f11.f64, l_f12.f64);
				l_f10.f64 = REX_FMULS(l_f12.f64, l_f10.f64);
				l_f8.f64 = double(float(std::fma(l_f8.f64, l_f9.f64, f31.f64)));
				l_f7.f64 = double(float(std::fma(l_f7.f64, l_f9.f64, l_f11.f64)));
				l_f5.f64 = double(float(std::fma(l_f5.f64, l_f6.f64, l_f10.f64)));
				l_f4.f64 = double(float(std::fma(l_f4.f64, l_f6.f64, l_f8.f64)));
				l_f3.f64 = double(float(std::fma(l_f3.f64, l_f6.f64, l_f7.f64)));
				l_f2.f64 = double(float(std::fma(l_f2.f64, l_f9.f64, l_f5.f64)));
				l_f1.f64 = REX_FADDS(l_f4.f64, l_f1.f64);
				temp.f32 = float(l_f1.f64);
				REX_STORE_D_U32(l_r10.u32, 16, temp.u32);
				f0.f64 = REX_FADDS(l_f3.f64, f0.f64);
				temp.f32 = float(f0.f64);
				REX_STORE_U32(l_r7.u32 + r11.u32, temp.u32);
				l_f13.f64 = REX_FADDS(l_f2.f64, l_f13.f64);
				temp.f32 = float(l_f13.f64);
				REX_STORE_D_U32(l_r10.u32, 24, temp.u32);
				l_f2.f64 = double(m[6]);
				l_f11.f64 = double(REX_LOAD_D_F32(r11.u32, 12));
				l_f10.f64 = double(REX_LOAD_D_F32(r11.u32, 8));
				l_f9.f64 = double(m[4]);
				l_f8.f64 = double(m[9]);
				l_f7.f64 = double(m[5]);
				l_f6.f64 = double(m[8]);
				l_f5.f64 = double(m[7]);
				l_f4.f64 = double(m[11]);
				l_f12.f64 = double(m[10]);
				l_f3.f64 = double(REX_LOAD_D_F32(r11.u32, 16));
				f0.f64 = REX_FMULS(l_f12.f64, l_f3.f64);
				l_f1.f64 = REX_FMULS(l_f2.f64, l_f3.f64);
				l_f13.f64 = double(m[0]);
				l_f8.f64 = double(float(std::fma(l_f8.f64, l_f11.f64, f0.f64)));
				l_f12.f64 = double(float(std::fma(l_f9.f64, l_f10.f64, l_f1.f64)));
				l_f9.f64 = double(m[1]);
				l_f6.f64 = double(float(std::fma(l_f10.f64, l_f6.f64, l_f8.f64)));
				l_f7.f64 = double(float(std::fma(l_f7.f64, l_f11.f64, l_f12.f64)));
				l_f4.f64 = REX_FADDS(l_f6.f64, l_f4.f64);
				l_f5.f64 = REX_FADDS(l_f7.f64, l_f5.f64);
				l_f2.f64 = REX_FMULS(l_f9.f64, l_f11.f64);
				l_f1.f64 = double(m[2]);
				f0.f64 = double(m[3]);
				l_r9.s64 = l_r9.s64 + 4;
				cr6.compare<uint32_t>(l_r9.u32, l_r8.u32, xer);
				l_f13.f64 = double(float(std::fma(l_f13.f64, l_f10.f64, l_f2.f64)));
				l_f12.f64 = double(float(std::fma(l_f1.f64, l_f3.f64, l_f13.f64)));
				l_f11.f64 = REX_FADDS(l_f12.f64, f0.f64);
				temp.f32 = float(l_f11.f64);
				REX_STORE_D_U32(l_r10.u32, 28, temp.u32);
				temp.f32 = float(l_f5.f64);
				REX_STORE_D_U32(l_r10.u32, 32, temp.u32);
				temp.f32 = float(l_f4.f64);
				REX_STORE_D_U32(l_r10.u32, 36, temp.u32);
				l_f10.f64 = double(REX_LOAD_D_F32(r11.u32, 28));
				l_f9.f64 = double(m[6]);
				l_f8.f64 = double(m[10]);
				l_f7.f64 = double(REX_LOAD_D_F32(r11.u32, 20));
				l_f6.f64 = double(m[0]);
				l_f5.f64 = double(m[4]);
				l_f4.f64 = double(m[9]);
				l_f3.f64 = double(m[2]);
				l_f2.f64 = double(m[5]);
				l_f1.f64 = double(m[8]);
				f0.f64 = double(m[3]);
				l_f13.f64 = double(m[7]);
				l_f12.f64 = double(m[11]);
				l_f11.f64 = double(REX_LOAD_D_F32(r11.u32, 24));
				r11.s64 = r11.s64 + 48;
				f31.f64 = double(m[1]);
				f31.f64 = REX_FMULS(f31.f64, l_f11.f64);
				l_f9.f64 = REX_FMULS(l_f9.f64, l_f10.f64);
				l_f8.f64 = REX_FMULS(l_f8.f64, l_f10.f64);
				l_f6.f64 = double(float(std::fma(l_f6.f64, l_f7.f64, f31.f64)));
				l_f5.f64 = double(float(std::fma(l_f5.f64, l_f7.f64, l_f9.f64)));
				l_f4.f64 = double(float(std::fma(l_f4.f64, l_f11.f64, l_f8.f64)));
				l_f3.f64 = double(float(std::fma(l_f3.f64, l_f10.f64, l_f6.f64)));
				l_f2.f64 = double(float(std::fma(l_f2.f64, l_f11.f64, l_f5.f64)));
				l_f1.f64 = double(float(std::fma(l_f7.f64, l_f1.f64, l_f4.f64)));
				f0.f64 = REX_FADDS(l_f3.f64, f0.f64);
				temp.f32 = float(f0.f64);
				REX_STORE_D_U32(l_r10.u32, 40, temp.u32);
				l_f13.f64 = REX_FADDS(l_f2.f64, l_f13.f64);
				temp.f32 = float(l_f13.f64);
				REX_STORE_D_U32(l_r10.u32, 44, temp.u32);
				l_f12.f64 = REX_FADDS(l_f1.f64, l_f12.f64);
				ea = 48 + l_r10.u32;
				temp.f32 = float(l_f12.f64);
				REX_STORE_U32(ea, temp.u32);
				l_r10.u32 = ea;
			} while (cr6.lt);
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(first), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check4) [[likely]] goto loc_82147648_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check4->out[0] = l_f1.u64;
			check4->out[1] = l_f2.u64;
			check4->out[2] = l_f3.u64;
			check4->out[3] = l_f4.u64;
			check4->out[4] = l_f5.u64;
			check4->out[5] = l_f6.u64;
			check4->out[6] = l_f7.u64;
			check4->out[7] = l_f8.u64;
			check4->out[8] = l_f9.u64;
			check4->out[9] = l_f10.u64;
			check4->out[10] = l_f11.u64;
			check4->out[11] = l_f12.u64;
			check4->out[12] = l_f13.u64;
			check4->out[13] = f0.u64;
			check4->out[14] = l_r9.u64;
			check4->out[15] = l_r10.u64;
			check4->out[16] = r11.u64;
			check4->out[17] = f31.u64;
			check4->out[18] = uint64_t(ea);
			check4->out[19] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check4, base);
			l_f1.u64 = check4->in[0];
			l_f2.u64 = check4->in[1];
			l_f3.u64 = check4->in[2];
			l_f4.u64 = check4->in[3];
			l_f5.u64 = check4->in[4];
			l_f6.u64 = check4->in[5];
			l_f7.u64 = check4->in[6];
			l_f8.u64 = check4->in[7];
			l_f9.u64 = check4->in[8];
			l_f10.u64 = check4->in[9];
			l_f11.u64 = check4->in[10];
			l_f12.u64 = check4->in[11];
			l_f13.u64 = check4->in[12];
			f0.u64 = check4->in[13];
			l_r9.u64 = check4->in[14];
			l_r10.u64 = check4->in[15];
			r11.u64 = check4->in[16];
			f31.u64 = check4->in[17];
			ea = uint32_t(check4->in[18]);
			temp.u64 = check4->in[19];
		}
	}
loc_82147648:
	REX_LOOP_BARRIER();
	REX_FLUSH_HOIST_KNOWN_OFF();
	f0.f64 = double(REX_LOAD_D_F32(r11.u32, -12));
	l_f13.f64 = double(REX_LOAD_D_F32(l_r3.u32, 4));
	l_f12.f64 = double(REX_LOAD_D_F32(r11.u32, -8));
	l_f11.f64 = REX_FMULS(l_f13.f64, f0.f64);
	l_f10.f64 = double(REX_LOAD_D_F32(l_r3.u32, 24));
	l_f9.f64 = double(REX_LOAD_D_F32(l_r3.u32, 40));
	l_f8.f64 = REX_FMULS(l_f10.f64, l_f12.f64);
	l_f7.f64 = REX_FMULS(l_f9.f64, l_f12.f64);
	l_f6.f64 = double(REX_LOAD_D_F32(r11.u32, -16));
	l_f5.f64 = double(REX_LOAD_D_F32(l_r3.u32, 0));
	l_f4.f64 = double(REX_LOAD_D_F32(l_r3.u32, 16));
	l_f3.f64 = double(REX_LOAD_D_F32(l_r3.u32, 36));
	l_f2.f64 = double(REX_LOAD_D_F32(l_r3.u32, 8));
	l_f1.f64 = double(REX_LOAD_D_F32(l_r3.u32, 20));
	l_f13.f64 = double(REX_LOAD_D_F32(l_r3.u32, 32));
	l_f11.f64 = double(float(std::fma(l_f5.f64, l_f6.f64, l_f11.f64)));
	l_f10.f64 = double(REX_LOAD_D_F32(l_r3.u32, 12));
	l_f9.f64 = double(REX_LOAD_D_F32(l_r3.u32, 28));
	l_f8.f64 = double(float(std::fma(l_f4.f64, l_f6.f64, l_f8.f64)));
	l_f5.f64 = double(REX_LOAD_D_F32(l_r3.u32, 44));
	l_f4.f64 = double(float(std::fma(l_f3.f64, f0.f64, l_f7.f64)));
	l_f3.f64 = double(float(std::fma(l_f2.f64, l_f12.f64, l_f11.f64)));
	l_f2.f64 = double(float(std::fma(l_f1.f64, f0.f64, l_f8.f64)));
	l_f1.f64 = double(float(std::fma(l_f6.f64, l_f13.f64, l_f4.f64)));
	f0.f64 = REX_FADDS(l_f3.f64, l_f10.f64);
	temp.f32 = float(f0.f64);
	REX_STORE_D_U32_C(l_r10.u32, 4, temp.u32);
	l_f13.f64 = REX_FADDS(l_f2.f64, l_f9.f64);
	temp.f32 = float(l_f13.f64);
	REX_STORE_D_U32_C(l_r10.u32, 8, temp.u32);
	l_f12.f64 = REX_FADDS(l_f1.f64, l_f5.f64);
	temp.f32 = float(l_f12.f64);
	REX_STORE_D_U32_C(l_r10.u32, 12, temp.u32);
	l_f11.f64 = double(REX_LOAD_D_F32(l_r3.u32, 16));
	l_f10.f64 = double(REX_LOAD_D_F32(l_r3.u32, 32));
	l_f9.f64 = double(REX_LOAD_D_F32(r11.u32, 0));
	l_f8.f64 = double(REX_LOAD_D_F32(l_r3.u32, 4));
	l_f7.f64 = double(REX_LOAD_D_F32(l_r3.u32, 20));
	l_f6.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
	l_f5.f64 = double(REX_LOAD_D_F32(l_r3.u32, 40));
	l_f4.f64 = double(REX_LOAD_D_F32(l_r3.u32, 8));
	l_f3.f64 = double(REX_LOAD_D_F32(l_r3.u32, 24));
	l_f2.f64 = double(REX_LOAD_D_F32(l_r3.u32, 36));
	l_f1.f64 = double(REX_LOAD_D_F32(l_r3.u32, 12));
	f0.f64 = double(REX_LOAD_D_F32(l_r3.u32, 28));
	l_f13.f64 = double(REX_LOAD_D_F32(l_r3.u32, 44));
	l_f12.f64 = double(REX_LOAD_D_F32(r11.u32, -4));
	f31.f64 = double(REX_LOAD_D_F32(l_r3.u32, 0));
	f31.f64 = REX_FMULS(f31.f64, l_f12.f64);
	l_f11.f64 = REX_FMULS(l_f11.f64, l_f12.f64);
	l_f10.f64 = REX_FMULS(l_f12.f64, l_f10.f64);
	l_f8.f64 = double(float(std::fma(l_f8.f64, l_f9.f64, f31.f64)));
	l_f7.f64 = double(float(std::fma(l_f7.f64, l_f9.f64, l_f11.f64)));
	l_f5.f64 = double(float(std::fma(l_f5.f64, l_f6.f64, l_f10.f64)));
	l_f4.f64 = double(float(std::fma(l_f4.f64, l_f6.f64, l_f8.f64)));
	l_f3.f64 = double(float(std::fma(l_f3.f64, l_f6.f64, l_f7.f64)));
	l_f2.f64 = double(float(std::fma(l_f2.f64, l_f9.f64, l_f5.f64)));
	l_f1.f64 = REX_FADDS(l_f4.f64, l_f1.f64);
	temp.f32 = float(l_f1.f64);
	REX_STORE_D_U32_C(l_r10.u32, 16, temp.u32);
	f0.f64 = REX_FADDS(l_f3.f64, f0.f64);
	temp.f32 = float(f0.f64);
	REX_STORE_U32_C(l_r7.u32 + r11.u32, temp.u32);
	l_f13.f64 = REX_FADDS(l_f2.f64, l_f13.f64);
	temp.f32 = float(l_f13.f64);
	REX_STORE_D_U32_C(l_r10.u32, 24, temp.u32);
	l_f2.f64 = double(REX_LOAD_D_F32(l_r3.u32, 24));
	l_f11.f64 = double(REX_LOAD_D_F32(r11.u32, 12));
	l_f10.f64 = double(REX_LOAD_D_F32(r11.u32, 8));
	l_f9.f64 = double(REX_LOAD_D_F32(l_r3.u32, 16));
	l_f8.f64 = double(REX_LOAD_D_F32(l_r3.u32, 36));
	l_f7.f64 = double(REX_LOAD_D_F32(l_r3.u32, 20));
	l_f6.f64 = double(REX_LOAD_D_F32(l_r3.u32, 32));
	l_f5.f64 = double(REX_LOAD_D_F32(l_r3.u32, 28));
	l_f4.f64 = double(REX_LOAD_D_F32(l_r3.u32, 44));
	l_f12.f64 = double(REX_LOAD_D_F32(l_r3.u32, 40));
	l_f3.f64 = double(REX_LOAD_D_F32(r11.u32, 16));
	f0.f64 = REX_FMULS(l_f12.f64, l_f3.f64);
	l_f1.f64 = REX_FMULS(l_f2.f64, l_f3.f64);
	l_f13.f64 = double(REX_LOAD_D_F32(l_r3.u32, 0));
	l_f8.f64 = double(float(std::fma(l_f8.f64, l_f11.f64, f0.f64)));
	l_f12.f64 = double(float(std::fma(l_f9.f64, l_f10.f64, l_f1.f64)));
	l_f9.f64 = double(REX_LOAD_D_F32(l_r3.u32, 4));
	l_f6.f64 = double(float(std::fma(l_f10.f64, l_f6.f64, l_f8.f64)));
	l_f7.f64 = double(float(std::fma(l_f7.f64, l_f11.f64, l_f12.f64)));
	l_f4.f64 = REX_FADDS(l_f6.f64, l_f4.f64);
	l_f5.f64 = REX_FADDS(l_f7.f64, l_f5.f64);
	l_f2.f64 = REX_FMULS(l_f9.f64, l_f11.f64);
	l_f1.f64 = double(REX_LOAD_D_F32(l_r3.u32, 8));
	f0.f64 = double(REX_LOAD_D_F32(l_r3.u32, 12));
	l_r9.s64 = l_r9.s64 + 4;
	cr6.compare<uint32_t>(l_r9.u32, l_r8.u32, xer);
	l_f13.f64 = double(float(std::fma(l_f13.f64, l_f10.f64, l_f2.f64)));
	l_f12.f64 = double(float(std::fma(l_f1.f64, l_f3.f64, l_f13.f64)));
	l_f11.f64 = REX_FADDS(l_f12.f64, f0.f64);
	temp.f32 = float(l_f11.f64);
	REX_STORE_D_U32_C(l_r10.u32, 28, temp.u32);
	temp.f32 = float(l_f5.f64);
	REX_STORE_D_U32_C(l_r10.u32, 32, temp.u32);
	temp.f32 = float(l_f4.f64);
	REX_STORE_D_U32_C(l_r10.u32, 36, temp.u32);
	l_f10.f64 = double(REX_LOAD_D_F32(r11.u32, 28));
	l_f9.f64 = double(REX_LOAD_D_F32(l_r3.u32, 24));
	l_f8.f64 = double(REX_LOAD_D_F32(l_r3.u32, 40));
	l_f7.f64 = double(REX_LOAD_D_F32(r11.u32, 20));
	l_f6.f64 = double(REX_LOAD_D_F32(l_r3.u32, 0));
	l_f5.f64 = double(REX_LOAD_D_F32(l_r3.u32, 16));
	l_f4.f64 = double(REX_LOAD_D_F32(l_r3.u32, 36));
	l_f3.f64 = double(REX_LOAD_D_F32(l_r3.u32, 8));
	l_f2.f64 = double(REX_LOAD_D_F32(l_r3.u32, 20));
	l_f1.f64 = double(REX_LOAD_D_F32(l_r3.u32, 32));
	f0.f64 = double(REX_LOAD_D_F32(l_r3.u32, 12));
	l_f13.f64 = double(REX_LOAD_D_F32(l_r3.u32, 28));
	l_f12.f64 = double(REX_LOAD_D_F32(l_r3.u32, 44));
	l_f11.f64 = double(REX_LOAD_D_F32(r11.u32, 24));
	r11.s64 = r11.s64 + 48;
	f31.f64 = double(REX_LOAD_D_F32(l_r3.u32, 4));
	f31.f64 = REX_FMULS(f31.f64, l_f11.f64);
	l_f9.f64 = REX_FMULS(l_f9.f64, l_f10.f64);
	l_f8.f64 = REX_FMULS(l_f8.f64, l_f10.f64);
	l_f6.f64 = double(float(std::fma(l_f6.f64, l_f7.f64, f31.f64)));
	l_f5.f64 = double(float(std::fma(l_f5.f64, l_f7.f64, l_f9.f64)));
	l_f4.f64 = double(float(std::fma(l_f4.f64, l_f11.f64, l_f8.f64)));
	l_f3.f64 = double(float(std::fma(l_f3.f64, l_f10.f64, l_f6.f64)));
	l_f2.f64 = double(float(std::fma(l_f2.f64, l_f11.f64, l_f5.f64)));
	l_f1.f64 = double(float(std::fma(l_f7.f64, l_f1.f64, l_f4.f64)));
	f0.f64 = REX_FADDS(l_f3.f64, f0.f64);
	temp.f32 = float(f0.f64);
	REX_STORE_D_U32_C(l_r10.u32, 40, temp.u32);
	l_f13.f64 = REX_FADDS(l_f2.f64, l_f13.f64);
	temp.f32 = float(l_f13.f64);
	REX_STORE_D_U32_C(l_r10.u32, 44, temp.u32);
	l_f12.f64 = REX_FADDS(l_f1.f64, l_f12.f64);
	ea = 48 + l_r10.u32;
	temp.f32 = float(l_f12.f64);
	REX_STORE_U32_C(ea, temp.u32);
	l_r10.u32 = ea;
	if (cr6.lt) goto loc_82147648;
	if (check4) [[unlikely]] {
		uint64_t want_[17];
		want_[0] = l_f1.u64;
		want_[1] = l_f2.u64;
		want_[2] = l_f3.u64;
		want_[3] = l_f4.u64;
		want_[4] = l_f5.u64;
		want_[5] = l_f6.u64;
		want_[6] = l_f7.u64;
		want_[7] = l_f8.u64;
		want_[8] = l_f9.u64;
		want_[9] = l_f10.u64;
		want_[10] = l_f11.u64;
		want_[11] = l_f12.u64;
		want_[12] = l_f13.u64;
		want_[13] = f0.u64;
		want_[14] = l_r9.u64;
		want_[15] = l_r10.u64;
		want_[16] = r11.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check4, base, want_, 17, "sub_82147628 fast points (four a pass)", checks_);
	}
loc_82147648_done:
loc_82147838:
	cr6.compare<uint32_t>(l_r9.u32, l_r6.u32, xer);
	if (!cr6.lt) goto loc_821478E8;
	r11.u64 = ((uint32_t)(l_r9.u32 << 1));
	l_r8.u64 = l_r6.u64 - l_r9.u64;
	l_r7.u64 = l_r9.u64 + r11.u64;
	l_r9.u64 = l_r5.u64 - l_r4.u64;
	r11.u64 = ((uint32_t)(l_r7.u32 << 2));
	l_r10.u64 = r11.u64 + l_r5.u64;
	ctr.u64 = l_r8.u64;
	r11.u64 = r11.u64 + l_r4.u64;
	l_r10.s64 = l_r10.s64 + -4;
	r11.s64 = r11.s64 + 4;
	ctx.fpscr.disableFlushMode(); // flush_hoist.py
	{
		// sa2-nx (2026-10-28): sub_82147628 fast points (the rest) where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the points (the copy's counter)
		const uint64_t first = uint32_t(l_r10.u32 + 4), bytes = 12ull * n;
		if (n != 0 && ::sa2::hot::fast_loop::Flat(first, bytes) && ::sa2::hot::fast_loop::Apart(first, bytes, l_r3.u32, 48) && ::sa2::hot::fast_loop::Flat(l_r3.u32, 48)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check1 = new ::sa2::hot::fast_loop::Check{};
				check1->Add(uint32_t(first), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check1, base);
				check1->in[0] = l_f1.u64;
				check1->in[1] = l_f2.u64;
				check1->in[2] = l_f3.u64;
				check1->in[3] = l_f4.u64;
				check1->in[4] = l_f5.u64;
				check1->in[5] = l_f6.u64;
				check1->in[6] = l_f7.u64;
				check1->in[7] = l_f8.u64;
				check1->in[8] = l_f9.u64;
				check1->in[9] = l_f10.u64;
				check1->in[10] = l_f11.u64;
				check1->in[11] = l_f12.u64;
				check1->in[12] = l_f13.u64;
				check1->in[13] = f0.u64;
				check1->in[14] = l_r10.u64;
				check1->in[15] = r11.u64;
				check1->in[16] = ctr.u64;
				check1->in[17] = uint64_t(ea);
				check1->in[18] = temp.u64;
			}
			// The matrix [r3, r3+48), reloaded by the copy for every point (no output lands on it).
			float m[12];
			for (uint32_t k = 0; k < 12; ++k) m[k] = REX_LOAD_D_F32(l_r3.u32, 4 * k);
			uint32_t left = n;
			do {
				REX_LOOP_BARRIER();
				REX_FLUSH_HOIST_KNOWN_OFF();
				l_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 0));
				l_f12.f64 = double(m[5]);
				f0.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
				l_f10.f64 = REX_FMULS(l_f12.f64, l_f13.f64);
				l_f11.f64 = double(m[2]);
				l_f9.f64 = double(m[9]);
				l_f8.f64 = REX_FMULS(l_f11.f64, f0.f64);
				l_f7.f64 = REX_FMULS(l_f9.f64, l_f13.f64);
				l_f5.f64 = double(m[6]);
				l_f6.f64 = double(REX_LOAD_D_F32(r11.u32, -4));
				l_f4.f64 = double(m[0]);
				l_f3.f64 = double(m[10]);
				l_f2.f64 = double(m[4]);
				l_f1.f64 = double(m[1]);
				l_f12.f64 = double(m[8]);
				l_f10.f64 = double(float(std::fma(l_f5.f64, f0.f64, l_f10.f64)));
				l_f11.f64 = double(m[7]);
				l_f9.f64 = double(m[3]);
				l_f8.f64 = double(float(std::fma(l_f4.f64, l_f6.f64, l_f8.f64)));
				l_f5.f64 = double(m[11]);
				l_f4.f64 = double(float(std::fma(l_f3.f64, f0.f64, l_f7.f64)));
				l_f3.f64 = double(float(std::fma(l_f2.f64, l_f6.f64, l_f10.f64)));
				l_f2.f64 = double(float(std::fma(l_f1.f64, l_f13.f64, l_f8.f64)));
				l_f1.f64 = double(float(std::fma(l_f6.f64, l_f12.f64, l_f4.f64)));
				f0.f64 = REX_FADDS(l_f3.f64, l_f11.f64);
				temp.f32 = float(f0.f64);
				REX_STORE_U32(l_r9.u32 + r11.u32, temp.u32);
				r11.s64 = r11.s64 + 12;
				l_f13.f64 = REX_FADDS(l_f2.f64, l_f9.f64);
				temp.f32 = float(l_f13.f64);
				REX_STORE_D_U32(l_r10.u32, 4, temp.u32);
				l_f12.f64 = REX_FADDS(l_f1.f64, l_f5.f64);
				ea = 12 + l_r10.u32;
				temp.f32 = float(l_f12.f64);
				REX_STORE_U32(ea, temp.u32);
				l_r10.u32 = ea;
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(first), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check1) [[likely]] goto loc_82147868_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check1->out[0] = l_f1.u64;
			check1->out[1] = l_f2.u64;
			check1->out[2] = l_f3.u64;
			check1->out[3] = l_f4.u64;
			check1->out[4] = l_f5.u64;
			check1->out[5] = l_f6.u64;
			check1->out[6] = l_f7.u64;
			check1->out[7] = l_f8.u64;
			check1->out[8] = l_f9.u64;
			check1->out[9] = l_f10.u64;
			check1->out[10] = l_f11.u64;
			check1->out[11] = l_f12.u64;
			check1->out[12] = l_f13.u64;
			check1->out[13] = f0.u64;
			check1->out[14] = l_r10.u64;
			check1->out[15] = r11.u64;
			check1->out[16] = ctr.u64;
			check1->out[17] = uint64_t(ea);
			check1->out[18] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check1, base);
			l_f1.u64 = check1->in[0];
			l_f2.u64 = check1->in[1];
			l_f3.u64 = check1->in[2];
			l_f4.u64 = check1->in[3];
			l_f5.u64 = check1->in[4];
			l_f6.u64 = check1->in[5];
			l_f7.u64 = check1->in[6];
			l_f8.u64 = check1->in[7];
			l_f9.u64 = check1->in[8];
			l_f10.u64 = check1->in[9];
			l_f11.u64 = check1->in[10];
			l_f12.u64 = check1->in[11];
			l_f13.u64 = check1->in[12];
			f0.u64 = check1->in[13];
			l_r10.u64 = check1->in[14];
			r11.u64 = check1->in[15];
			ctr.u64 = check1->in[16];
			ea = uint32_t(check1->in[17]);
			temp.u64 = check1->in[18];
		}
	}
loc_82147868:
	REX_LOOP_BARRIER();
	REX_FLUSH_HOIST_KNOWN_OFF();
	l_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 0));
	l_f12.f64 = double(REX_LOAD_D_F32(l_r3.u32, 20));
	f0.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
	l_f10.f64 = REX_FMULS(l_f12.f64, l_f13.f64);
	l_f11.f64 = double(REX_LOAD_D_F32(l_r3.u32, 8));
	l_f9.f64 = double(REX_LOAD_D_F32(l_r3.u32, 36));
	l_f8.f64 = REX_FMULS(l_f11.f64, f0.f64);
	l_f7.f64 = REX_FMULS(l_f9.f64, l_f13.f64);
	l_f5.f64 = double(REX_LOAD_D_F32(l_r3.u32, 24));
	l_f6.f64 = double(REX_LOAD_D_F32(r11.u32, -4));
	l_f4.f64 = double(REX_LOAD_D_F32(l_r3.u32, 0));
	l_f3.f64 = double(REX_LOAD_D_F32(l_r3.u32, 40));
	l_f2.f64 = double(REX_LOAD_D_F32(l_r3.u32, 16));
	l_f1.f64 = double(REX_LOAD_D_F32(l_r3.u32, 4));
	l_f12.f64 = double(REX_LOAD_D_F32(l_r3.u32, 32));
	l_f10.f64 = double(float(std::fma(l_f5.f64, f0.f64, l_f10.f64)));
	l_f11.f64 = double(REX_LOAD_D_F32(l_r3.u32, 28));
	l_f9.f64 = double(REX_LOAD_D_F32(l_r3.u32, 12));
	l_f8.f64 = double(float(std::fma(l_f4.f64, l_f6.f64, l_f8.f64)));
	l_f5.f64 = double(REX_LOAD_D_F32(l_r3.u32, 44));
	l_f4.f64 = double(float(std::fma(l_f3.f64, f0.f64, l_f7.f64)));
	l_f3.f64 = double(float(std::fma(l_f2.f64, l_f6.f64, l_f10.f64)));
	l_f2.f64 = double(float(std::fma(l_f1.f64, l_f13.f64, l_f8.f64)));
	l_f1.f64 = double(float(std::fma(l_f6.f64, l_f12.f64, l_f4.f64)));
	f0.f64 = REX_FADDS(l_f3.f64, l_f11.f64);
	temp.f32 = float(f0.f64);
	REX_STORE_U32_C(l_r9.u32 + r11.u32, temp.u32);
	r11.s64 = r11.s64 + 12;
	l_f13.f64 = REX_FADDS(l_f2.f64, l_f9.f64);
	temp.f32 = float(l_f13.f64);
	REX_STORE_D_U32_C(l_r10.u32, 4, temp.u32);
	l_f12.f64 = REX_FADDS(l_f1.f64, l_f5.f64);
	ea = 12 + l_r10.u32;
	temp.f32 = float(l_f12.f64);
	REX_STORE_U32_C(ea, temp.u32);
	l_r10.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82147868;
	if (check1) [[unlikely]] {
		uint64_t want_[17];
		want_[0] = l_f1.u64;
		want_[1] = l_f2.u64;
		want_[2] = l_f3.u64;
		want_[3] = l_f4.u64;
		want_[4] = l_f5.u64;
		want_[5] = l_f6.u64;
		want_[6] = l_f7.u64;
		want_[7] = l_f8.u64;
		want_[8] = l_f9.u64;
		want_[9] = l_f10.u64;
		want_[10] = l_f11.u64;
		want_[11] = l_f12.u64;
		want_[12] = l_f13.u64;
		want_[13] = f0.u64;
		want_[14] = l_r10.u64;
		want_[15] = r11.u64;
		want_[16] = ctr.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check1, base, want_, 17, "sub_82147628 fast points (the rest)", checks_);
	}
loc_82147868_done:
loc_821478E8:
	f31.u64 = REX_LOAD_D_U64(l_r1.u32, -8);
	{ do { ctx.f1 = l_f1; ctx.f2 = l_f2; ctx.f3 = l_f3; ctx.f4 = l_f4; ctx.f5 = l_f5; ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_point_transform
