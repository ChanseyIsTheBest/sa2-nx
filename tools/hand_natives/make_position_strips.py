"""Writes app/src/cpu/hot/n_position_strips.h: the localized copy of sub_8214E2B0 with a fast inner (vertex) loop."""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/') + '/'
src = open(ROOT + 'app/generated/hot/n_8214E2B0.h', encoding='utf-8').read()

loop_start = 'loc_8214E340:\n'
loop_end = '\tif (!cr0.eq) goto loc_8214E340;\n'
assert src.count(loop_start) == 1 and src.count(loop_end) == 1
i = src.index(loop_start)
j = src.index(loop_end) + len(loop_end)
copy_loop = src[i:j]
# The copy's loop as the fast loop expects it (a codegen change must re-audit the fast loop).
expect = '''loc_8214E340:
	REX_LOOP_BARRIER();
	ea = 6 + r30.u32;
	ll_r10.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	xer.ca = ll_r7.u32 > 0;
	ll_r7.s64 = ll_r7.s64 + -1;
	cr0.compare<int32_t>(ll_r7.s32, 0, xer);
	ll_r8.u64 = REX_LOAD_D_U32(r31.u32, 0);
	ll_r9.s64 = ll_r10.s16;
	ll_r10.u64 = ll_r8.u64 + r11.u64;
	ll_r9.u64 = ((uint32_t)(ll_r9.u32 << 5));
	ll_r9.u64 = ll_r9.u64 + r29.u64;
	fb0.u32 = REX_LOAD_D_U32(ll_r9.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ctx.fpscr.disableFlushMode();
	ll_f13.f64 = double(REX_LOAD_D_F32(ll_r9.u32, 4));
	fb12.u32 = REX_LOAD_D_U32(ll_r9.u32, 0);
	ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_U32_C(ll_r8.u32 + r11.u32, temp.u32);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 4, temp.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	r11.s64 = r11.s64 + 12;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (!cr0.eq) goto loc_8214E340;
'''
assert copy_loop == expect, copy_loop
# r11 at the loop: the cursor [r31+20], loaded (empty descriptor) or computed and stored (open one) just above.
for line in ('\tr11.u64 = REX_LOAD_D_U32(r31.u32, 20);\n\tcr6.compare<uint32_t>(l_r8.u32, 0, xer);\n',
             '\tr11.u64 = l_r10.u64 + r11.u64;\n\tREX_STORE_D_U32_C(r31.u32, 20, r11.u32);\n\tgoto loc_8214E334;\n',
             '\tr31.s64 = l_r10.s64 + -3848;\n'):
    assert src.count(line) == 1, line

fast = r'''	{
		// sa2-nx (2026-10-28): the strip's vertices by the fast loop where it applies (the header's conditions), else
		// by the copy's loop below.
		const uint32_t n = ll_r7.u32;                          // the vertex count (the loop's counter)
		const uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);       // the vertex buffer
		const uint64_t cur0 = r11.u64;                         // its cursor, [r31+20] when below 4 GB
		const uint64_t dst = uint64_t(buf) + cur0, bytes = 12ull * n;
		const uint64_t desc = r31.u32;
		if (n != 0 && cur0 + bytes < 0x100000000ull && dst + bytes <= 0x100000000ull &&
		    (dst + bytes <= desc || desc + 24 <= dst)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				strip_check = new StripCheck{{uint32_t(dst), r31.u32 + 20}, {uint32_t(bytes), 4}};
				for (int k = 0; k < 2; ++k) Save(base, strip_check->at[k], strip_check->len[k], strip_check->before[k]);
				SA2_STRIP_REGS(strip_check->in, =);
			}
			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				ea = 6 + r30.u32;
				ll_r10.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				ll_r7.s64 = ll_r7.s64 + -1;
				ll_r8.u64 = buf;
				ll_r9.s64 = ll_r10.s16;
				ll_r10.u64 = ll_r8.u64 + r11.u64;
				ll_r9.u64 = ((uint32_t)(ll_r9.u32 << 5));
				ll_r9.u64 = ll_r9.u64 + r29.u64;
				fb0.u32 = REX_LOAD_D_U32(ll_r9.u32, 8);
				ll_f13.f64 = double(REX_LOAD_D_F32(ll_r9.u32, 4));
				fb12.u32 = REX_LOAD_D_U32(ll_r9.u32, 0);
				ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
				temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
				REX_STORE_U32(ll_r8.u32 + r11.u32, temp.u32);
				ea = 4 + ll_r10.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r10.u32 = ea;
				temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
				REX_STORE_D_U32(ll_r10.u32, 4, temp.u32);
				cur += 12;
				r11.u64 = cur;  // the copy's [r31+20] + 12, stored back
			} while (ll_r7.s32 != 0);
			// The cursor and the cards: the pages of every store's first byte (the stores tile [dst, dst + bytes) four
			// bytes apart), as the copy's stores mark.
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));
#endif
			if (!strip_check) [[likely]] goto loc_8214E340_done;
			// Verify mode: keep the fast loop's result, put the inputs back, and run the copy's loop to compare.
			SA2_STRIP_REGS(strip_check->out, =);
			for (int k = 0; k < 2; ++k) {
				Save(base, strip_check->at[k], strip_check->len[k], strip_check->fast[k]);
				Restore(base, strip_check->at[k], strip_check->before[k]);
			}
			SA2_STRIP_REGS_BACK(strip_check->in);
		}
	}
'''

check = r'''	if (strip_check) [[unlikely]] {
		uint64_t want[kStripRegs];
		SA2_STRIP_REGS(want, =);
		bool same = std::equal(want, want + kStripRegs, strip_check->out);
		std::vector<uint8_t> now;
		for (int k = 0; k < 2; ++k) {
			Save(base, strip_check->at[k], strip_check->len[k], now);
			same = same && now == strip_check->fast[k];
		}
		::sa2::enh::VerifyCheck();
		static std::atomic<uint32_t> checks_{0};
		const uint32_t count_ = checks_.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count_ == 1 || count_ == 4096 || count_ == 262144)
			REXLOG_INFO("[hot] sub_8214E2B0 fast vertex loop: {} checks against the copy's loop", count_);
		if (!same) {
			::sa2::enh::VerifyDifference();
			REXLOG_ERROR("[hot] DIFFERENCE: sub_8214E2B0's fast vertex loop ({} vertices to {:08X}) differs from the copy's "
			             "loop; the copy's result stands", strip_check->len[0] / 12, strip_check->at[0]);
		}
		delete strip_check;
		strip_check = nullptr;
	}
loc_8214E340_done:
'''

body = src[:i] + fast + copy_loop + check + src[j:]
decl = '\tuint32_t ea{};\n'
assert body.count(decl) == 1
body = body.replace(decl, decl + '\tStripCheck* strip_check = nullptr;  // verify mode only\n')
assert body.count('namespace sa2::hot::n_8214E2B0') == 2
body = body.replace('namespace sa2::hot::n_8214E2B0 {\n', r'''namespace sa2::hot::n_position_strips {

// Verify mode: the guest bytes the fast loop writes (the vertices, the cursor word) and the registers either loop
// writes, saved to run the copy's loop on the same inputs and compare.
struct StripCheck {
  uint32_t at[2], len[2];
  std::vector<uint8_t> before[2], fast[2];
  uint64_t in[10], out[10];
};
inline constexpr int kStripRegs = 8;  // compared: the first 8 (f0 and ea are the copy's dead locals after it)
// (Named one by one: an array of their addresses would make them address-taken, kept in memory in the fast loop too.)
#define SA2_STRIP_REGS(a, op)                                                                                  \
  do {                                                                                                         \
    (a)[0] op ll_r7.u64; (a)[1] op ll_r8.u64; (a)[2] op ll_r9.u64; (a)[3] op ll_r10.u64; (a)[4] op ll_f12.u64; \
    (a)[5] op ll_f13.u64; (a)[6] op r30.u64; (a)[7] op r11.u64; (a)[8] op f0.u64; (a)[9] op uint64_t(ea);      \
  } while (0)
#define SA2_STRIP_REGS_BACK(a)                                                                                 \
  do {                                                                                                         \
    ll_r7.u64 = (a)[0]; ll_r8.u64 = (a)[1]; ll_r9.u64 = (a)[2]; ll_r10.u64 = (a)[3]; ll_f12.u64 = (a)[4];      \
    ll_f13.u64 = (a)[5]; r30.u64 = (a)[6]; r11.u64 = (a)[7]; f0.u64 = (a)[8]; ea = uint32_t((a)[9]);           \
  } while (0)
inline void Save(const uint8_t* base, uint32_t a, uint32_t len, std::vector<uint8_t>& out) {
  out.resize(len);
  for (uint32_t k = 0; k < len; ++k) out[k] = *Raw(base, a + k);
}
inline void Restore(uint8_t* base, uint32_t a, const std::vector<uint8_t>& in) {
  for (uint32_t k = 0; k < in.size(); ++k) *Raw(base, a + k) = in[k];
}

''', 1)
body = body.replace('}  // namespace sa2::hot::n_8214E2B0', '#undef SA2_STRIP_REGS\n#undef SA2_STRIP_REGS_BACK\n\n}  // namespace sa2::hot::n_position_strips')
assert 'n_8214E2B0' not in body.split('#pragma once', 1)[1]

start = body.index('#pragma once')
pragma = '#pragma once\n'
body = body[:start] + pragma + ('\n#include <algorithm>\n#include <atomic>\n#include <vector>\n\n#include <rex/logging.h>\n\n'
                                '#include "../sa2_enhancements.h"\n#include "../sa2_hot_common.h"\n') + body[start + len(pragma):]
header = r'''// sa2-nx (2026-10-28): hand-written sub_8214E2B0 (1.3 % of the console session's guest instructions): a model's
// triangle strips into the vertex buffer of the descriptor at 0x8389F0F8 (r31), positions only, 12 bytes a vertex (x,
// y, z of the 32-byte vertex its index names); it opens each strip itself (sub_82139B48's code, inline) and closes it
// with sub_82139C90. The position-only sibling of sub_8214E3F0 (hot/n_strip_builder.h). Made from its localized copy
// (app/generated/hot/n_8214E2B0.h), which stays its reference: re-run tools/hand_natives/make_position_strips.py after
// the copy changes (it checks the loop it replaces statement by statement).
//
// Optimisation: the copy's inner loop reloads the buffer base [r31+0] and the cursor [r31+20] and stores the cursor
// back at every vertex, and marks a card at each of its four stores: the guest memory writes keep GCC from holding
// anything in a register across them. The fast loop holds the base and the cursor in registers, stores the cursor once
// at the end, and marks the vertices' cards once.
//
// Audit: it applies when the count n != 0, the cursor r11 (as the copy holds it at the loop: [r31+20], loaded or just
// stored) is below 4 GB and does not wrap in the loop, and the vertices [dst, dst + 12n) (dst = base + cursor) neither
// wrap at 4 GB nor overlap the descriptor [r31, r31+24): then the copy's reloads read what the fast loop holds and its
// intermediate cursor stores are overwritten by the last. Every other statement is the copy's, in its order, with its
// loads and stores of the vertex data and the strip stream in the same order (so a buffer overlapping them reads what
// the copy reads). The registers the loop leaves (r7-r10, f12, f13, r30, r11) are the copy's: the same statements of
// the last vertex; its cr0 / xer / f0 / ea are locals written before any read after the loop. The copy's
// disableFlushMode at every vertex runs once (nothing in the loop changes the mode). The cards (one per store: the page
// of its first byte) are the same pages, zeroed after the data, as the copy zeroes each after its store. Otherwise
// (n == 0 or a range above) the copy's loop runs.
//
// The function calls sub_82139C90, so the hook's guard cannot run both versions; in verify mode (sa2_cpu_verify) the
// fast loop checks itself instead: every strip it takes is run by both loops on the same inputs (the written bytes and
// registers saved and put back), the copy's result kept, and any difference counted in the [verify] line and logged
// as "[hot] DIFFERENCE".
'''
open(ROOT + 'app/src/cpu/hot/n_position_strips.h', 'w', encoding='utf-8', newline='\n').write(header + body[start:])
print('n_position_strips.h written')
