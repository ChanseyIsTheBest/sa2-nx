"""Writes app/src/cpu/hot/n_dxt1_block.h: the localized copy of sub_82833568 with a fast texel loop."""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/') + '/'
src = open(ROOT + 'app/generated/hot/n_82833568.h', encoding='utf-8').read()

before = '\tctr.u64 = l_r10.u64;\nloc_8283383C:\n'
loop_end = '\tif (ctr.u32 != 0) goto loc_8283383C;\n'
assert src.count(before) == 1 and src.count(loop_end) == 1
i = src.index(before) + len('\tctr.u64 = l_r10.u64;\n')
j = src.index(loop_end) + len(loop_end)
copy_loop = src[i:j]
# The copy's loop as the fast loop expects it (a codegen change must re-audit the fast loop).
expect = '''loc_8283383C:
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
'''
assert copy_loop == expect, copy_loop
setup = '''loc_82833828:
	l_r10.s64 = 16;
	l_r9.u64 = REX_LOAD_D_U32(l_r4.u32, 4);
	r11.s64 = l_r3.s64 + -4;
	l_r9.u64 = __builtin_rotateleft32(l_r9.u32, 16);
	ctr.u64 = l_r10.u64;
'''
assert src.count(setup) == 1

fast = r'''	{
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
'''

check = r'''	if (block_check) [[unlikely]] {
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
'''

body = src[:i] + fast + copy_loop + check + src[j:]
decl = '\tuint32_t ea{};\n'
assert body.count(decl) == 1
body = body.replace(decl, decl + '\tBlockCheck* block_check = nullptr;  // verify mode only\n')
assert body.count('namespace sa2::hot::n_82833568') == 2
body = body.replace('namespace sa2::hot::n_82833568 {\n', r'''namespace sa2::hot::n_dxt1_block {

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

''', 1)
body = body.replace('}  // namespace sa2::hot::n_82833568', '#undef SA2_BLOCK_REGS\n#undef SA2_BLOCK_REGS_BACK\n\n}  // namespace sa2::hot::n_dxt1_block')
assert 'n_82833568' not in body.split('#pragma once', 1)[1]

start = body.index('#pragma once')
pragma = '#pragma once\n'
body = body[:start] + pragma + ('\n#include <algorithm>\n#include <atomic>\n#include <cstring>\n#include <vector>\n\n#include <rex/logging.h>\n\n'
                                '#include "../sa2_enhancements.h"\n#include "../sa2_hot_common.h"\n') + body[start + len(pragma):]
header = r'''// sa2-nx (2026-10-28): hand-written sub_82833568, the hottest guest function of the game thread in console gameplay
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
'''
open(ROOT + 'app/src/cpu/hot/n_dxt1_block.h', 'w', encoding='utf-8', newline='\n').write(header + body[start:])
print('n_dxt1_block.h written')
