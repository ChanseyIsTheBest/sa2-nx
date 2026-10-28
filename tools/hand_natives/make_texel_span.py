"""Writes app/src/cpu/hot/n_texel_span.h: the localized copy of sub_828243A0 with fast texel-copy and colour-key loops."""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/') + '/'
src = open(ROOT + 'app/generated/hot/n_828243A0.h', encoding='utf-8').read()

# --- the texel copy (loc_828244B8) ---
copy_expect = '''loc_828244B8:
	REX_LOOP_BARRIER();
	ll_r9.u64 = (r11.u32 & 0xFFFFFFFCu);
	ll_r10.u64 = REX_LOAD_D_U32(r31.u32, 240);
	ll_r7.u64 = r11.u64;
	ll_r9.u64 = ll_r9.u64 | ll_r8.u64;
	ll_r6.u64 = r27.u64;
	ll_r7.u64 = ((uint32_t)(ll_r9.u32 << 2)) | (ll_r7.u64 & 0xFFFFFFFF00000003);
	r27.s64 = r27.s64 + 16;
	ll_r9.u64 = ((uint32_t)(ll_r7.u32 << 4));
	r11.s64 = r11.s64 + 1;
	ll_r10.u64 = ll_r9.u64 + ll_r10.u64;
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 0);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r6.u32, 0, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 4);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r6.u32, 4, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r6.u32, 8, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 12);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r6.u32, 12, temp.u32);
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_828244B8;
'''
copy_entry = '''	cr6.compare<uint32_t>(r11.u32, l_r10.u32, xer);
	if (!cr6.lt) goto loc_82824504;
	l_r10.u64 = l_r10.u64 - r11.u64;
	ctr.u64 = l_r10.u64;
'''
assert src.count(copy_expect) == 1 and src.count(copy_entry) == 1

copy_fast = r'''	{
		// sa2-nx (2026-10-28): the span's texels by the fast copy where it applies (the header's conditions), else by
		// the copy's loop below.
		const uint32_t n = ctr.u32;                          // the texel count (>= 1: r11 < r10 above)
		const uint32_t tex = REX_LOAD_D_U32(r31.u32, 240);   // the decoded blocks
		const uint64_t dst = r27.u32, bytes = 16ull * n, held = uint64_t(r31.u32) + 240;
		if (n != 0 && dst + bytes <= 0x100000000ull && (dst + bytes <= held || held + 4 <= dst)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				copy_check = new SpanCheck{{uint32_t(dst), 0}, {uint32_t(bytes), 0}};
				Save(base, copy_check->at[0], copy_check->len[0], copy_check->before[0]);
				SA2_COPY_REGS(copy_check->in, =);
			}
			uint32_t left = n;
			do {
				REX_LOOP_BARRIER();
				ll_r9.u64 = (r11.u32 & 0xFFFFFFFCu);
				ll_r10.u64 = tex;
				ll_r7.u64 = r11.u64;
				ll_r9.u64 = ll_r9.u64 | ll_r8.u64;
				ll_r6.u64 = r27.u64;
				ll_r7.u64 = ((uint32_t)(ll_r9.u32 << 2)) | (ll_r7.u64 & 0xFFFFFFFF00000003);
				r27.s64 = r27.s64 + 16;
				ll_r9.u64 = ((uint32_t)(ll_r7.u32 << 4));
				r11.s64 = r11.s64 + 1;
				ll_r10.u64 = ll_r9.u64 + ll_r10.u64;
				// The copy's four loads and stores in its order (a span overlapping the blocks reads what it reads).
				REX_STORE_D_U32(ll_r6.u32, 0, rex::ppc::QuietSingleBits(REX_LOAD_D_U32(ll_r10.u32, 0)));
				REX_STORE_D_U32(ll_r6.u32, 4, rex::ppc::QuietSingleBits(REX_LOAD_D_U32(ll_r10.u32, 4)));
				REX_STORE_D_U32(ll_r6.u32, 8, rex::ppc::QuietSingleBits(REX_LOAD_D_U32(ll_r10.u32, 8)));
				REX_STORE_D_U32(ll_r6.u32, 12, rex::ppc::QuietSingleBits(REX_LOAD_D_U32(ll_r10.u32, 12)));
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));  // the pages of the stores' first bytes, [dst, dst + bytes - 4]
#endif
			if (!copy_check) [[likely]] goto loc_828244B8_done;
			// Verify mode: keep the fast loop's result, put the inputs back, and run the copy's loop to compare.
			SA2_COPY_REGS(copy_check->out, =);
			Save(base, copy_check->at[0], copy_check->len[0], copy_check->fast[0]);
			Restore(base, copy_check->at[0], copy_check->before[0]);
			SA2_COPY_REGS_BACK(copy_check->in);
		}
	}
'''
copy_tail = r'''	if (copy_check) [[unlikely]] {
		uint64_t want[kCopyRegs];
		SA2_COPY_REGS(want, =);
		std::vector<uint8_t> now;
		Save(base, copy_check->at[0], copy_check->len[0], now);
		const bool same = std::equal(want, want + kCopyRegs, copy_check->out) && now == copy_check->fast[0];
		::sa2::enh::VerifyCheck();
		static std::atomic<uint32_t> checks_{0};
		const uint32_t count_ = checks_.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count_ == 1 || count_ == 4096 || count_ == 262144)
			REXLOG_INFO("[hot] sub_828243A0 fast texel copy: {} checks against the copy's loop", count_);
		if (!same) {
			::sa2::enh::VerifyDifference();
			REXLOG_ERROR("[hot] DIFFERENCE: sub_828243A0's fast texel copy ({} texels to {:08X}) differs from the copy's "
			             "loop; the copy's result stands", copy_check->len[0] / 16, copy_check->at[0]);
		}
		delete copy_check;
		copy_check = nullptr;
	}
loc_828244B8_done:
'''

# --- the colour key (loc_8282456C) ---
key_start = 'loc_8282456C:\n'
key_end = '\tif (ctr.u32 != 0) goto loc_8282456C;\n'
assert src.count(key_start) == 1 and src.count(key_end) == 1
ki = src.index(key_start)
kj = src.index(key_end) + len(key_end)
key_expect = '''loc_8282456C:
	REX_LOOP_BARRIER();
	REX_FLUSH_KNOWN_OFF();
	ll_f8.f64 = double(REX_LOAD_D_F32(r31.u32, 128));
	ll_f9.f64 = double(REX_LOAD_D_F32(r27.u32, 12));
	ll_f9.f64 = double(float(std::fma(ll_f9.f64, ll_f8.f64, f0.f64)));
	ll_f8.f64 = double(REX_LOAD_D_F32(r27.u32, 4));
	ll_f8.f64 = double(float(std::fma(ll_f8.f64, ll_f11.f64, f0.f64)));
	ll_f7.f64 = double(REX_LOAD_D_F32(r27.u32, 8));
	ll_f7.f64 = double(float(std::fma(ll_f7.f64, ll_f13.f64, f0.f64)));
	ll_f5.f64 = double(REX_LOAD_D_F32(r27.u32, 0));
	ll_f5.f64 = double(float(std::fma(ll_f5.f64, ll_f13.f64, f0.f64)));
	ll_f4.f64 = double(REX_LOAD_D_F32(r31.u32, 132));
	ll_f3.f64 = double(REX_LOAD_D_F32(r31.u32, 36));
	ll_f9.s64 = rex::ppc::fctiwz(ll_f9.f64);
	REX_STORE_D_U64(ll_r1.u32, 80, ll_f9.u64);
	r11.s64 = int32_t(REX_LOAD_D_U32(ll_r1.u32, 84));
	ll_f9.s64 = rex::ppc::fctiwz(ll_f8.f64);
	REX_STORE_D_U64(ll_r1.u32, 80, ll_f9.u64);
	ll_r10.s64 = int32_t(REX_LOAD_D_U32(ll_r1.u32, 84));
	ll_f9.s64 = rex::ppc::fctiwz(ll_f7.f64);
	REX_STORE_D_U64(ll_r1.u32, 80, ll_f9.u64);
	ll_r9.s64 = int32_t(REX_LOAD_D_U32(ll_r1.u32, 84));
	ll_f9.s64 = rex::ppc::fctiwz(ll_f5.f64);
	REX_STORE_D_U64(ll_r1.u32, 80, ll_f9.u64);
	ll_r8.s64 = int32_t(REX_LOAD_D_U32(ll_r1.u32, 84));
	REX_STORE_D_U64(ll_r1.u32, 112, ll_r10.u64);
	ll_f5.u64 = REX_LOAD_D_U64(ll_r1.u32, 112);
	REX_STORE_D_U64(ll_r1.u32, 88, r11.u64);
	REX_STORE_D_U64(ll_r1.u32, 104, ll_r8.u64);
	ll_f9.u64 = REX_LOAD_D_U64(ll_r1.u32, 88);
	REX_STORE_D_U64(ll_r1.u32, 96, ll_r9.u64);
	ll_f7.u64 = REX_LOAD_D_U64(ll_r1.u32, 104);
	ll_f9.f64 = double(ll_f9.s64);
	ll_f7.f64 = double(ll_f7.s64);
	ll_f8.u64 = REX_LOAD_D_U64(ll_r1.u32, 96);
	ll_f8.f64 = double(ll_f8.s64);
	r11.u64 = ll_r9.u64;
	ll_f9.f64 = double(float(ll_f9.f64));
	ll_f2.f64 = double(float(ll_f7.f64));
	ll_f5.f64 = double(ll_f5.s64);
	ll_f8.f64 = double(float(ll_f8.f64));
	ll_f7.f64 = REX_FMULS(ll_f9.f64, ll_f4.f64);
	ll_f4.f64 = REX_FMULS(ll_f2.f64, ll_f12.f64);
	ll_f5.f64 = double(float(ll_f5.f64));
	ll_f8.f64 = REX_FMULS(ll_f8.f64, ll_f12.f64);
	cr6.compare(ll_f4.f64, ll_f3.f64);
	ll_f9.f64 = REX_FMULS(ll_f5.f64, ll_f10.f64);
	if (!cr6.eq) goto loc_82824658;
	ll_f5.f64 = double(REX_LOAD_D_F32(r31.u32, 40));
	cr6.compare(ll_f9.f64, ll_f5.f64);
	if (!cr6.eq) goto loc_82824658;
	ll_f9.f64 = double(REX_LOAD_D_F32(r31.u32, 44));
	cr6.compare(ll_f8.f64, ll_f9.f64);
	if (!cr6.eq) goto loc_82824658;
	ll_f9.f64 = double(REX_LOAD_D_F32(r31.u32, 48));
	cr6.compare(ll_f7.f64, ll_f9.f64);
	if (!cr6.eq) goto loc_82824658;
	temp.f32 = float(ll_f6.f64);
	REX_STORE_D_U32_C(r27.u32, 0, temp.u32);
	temp.f32 = float(ll_f6.f64);
	REX_STORE_D_U32_C(r27.u32, 4, temp.u32);
	temp.f32 = float(ll_f6.f64);
	REX_STORE_D_U32_C(r27.u32, 8, temp.u32);
	temp.f32 = float(ll_f6.f64);
	REX_STORE_D_U32_C(r27.u32, 12, temp.u32);
loc_82824658:
	r27.s64 = r27.s64 + 16;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_8282456C;
'''
assert src[ki:kj] == key_expect, src[ki:kj]
key_entry = '''	ctr.u64 = r11.u64;
	r11.s64 = -2113929216;
	temp.u32 = REX_RO_U32(0x8200154C, 0x3F000000);
	f0.f64 = double(temp.f32);
'''
assert src.count(key_entry) == 1

key_fast = r'''	{
		// sa2-nx (2026-10-28): the colour key by the fast loop where it applies (the header's conditions), else by the
		// copy's loop below.
		const uint32_t n = ctr.u32;  // the span's texel count (>= 1)
		const uint64_t dst = r27.u32, bytes = 16ull * n, fields = uint64_t(r31.u32) + 36,
		               scales = uint64_t(r31.u32) + 128, slots = uint32_t(ll_r1.u32 + 80);
		if (n != 0 && dst + bytes <= 0x100000000ull && (dst + bytes <= fields || fields + 16 <= dst) &&
		    (dst + bytes <= scales || scales + 8 <= dst) && (dst + bytes <= slots || slots + 40 <= dst)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				key_check = new SpanCheck{{uint32_t(dst), uint32_t(slots)}, {uint32_t(bytes), 40}};
				for (int k = 0; k < 2; ++k) Save(base, key_check->at[k], key_check->len[k], key_check->before[k]);
				SA2_KEY_REGS(key_check->in, =);
			}
			// The texture's fields the copy reloads at every texel: two scales [r31+128], [r31+132] and the key's four
			// components [r31+36..48] (nothing the loop stores lands on them).
			const float scale128 = REX_LOAD_D_F32(r31.u32, 128), scale132 = REX_LOAD_D_F32(r31.u32, 132);
			const float key36 = REX_LOAD_D_F32(r31.u32, 36), key40 = REX_LOAD_D_F32(r31.u32, 40);
			const float key44 = REX_LOAD_D_F32(r31.u32, 44), key48 = REX_LOAD_D_F32(r31.u32, 48);
			uint64_t slot80 = 0, slot88 = 0;
			uint32_t left = n;
			REX_FLUSH_KNOWN_OFF();
			do {
				REX_LOOP_BARRIER();
				ll_f8.f64 = double(scale128);
				ll_f9.f64 = double(REX_LOAD_D_F32(r27.u32, 12));
				ll_f9.f64 = double(float(std::fma(ll_f9.f64, ll_f8.f64, f0.f64)));
				ll_f8.f64 = double(REX_LOAD_D_F32(r27.u32, 4));
				ll_f8.f64 = double(float(std::fma(ll_f8.f64, ll_f11.f64, f0.f64)));
				ll_f7.f64 = double(REX_LOAD_D_F32(r27.u32, 8));
				ll_f7.f64 = double(float(std::fma(ll_f7.f64, ll_f13.f64, f0.f64)));
				ll_f5.f64 = double(REX_LOAD_D_F32(r27.u32, 0));
				ll_f5.f64 = double(float(std::fma(ll_f5.f64, ll_f13.f64, f0.f64)));
				ll_f4.f64 = double(scale132);
				ll_f3.f64 = double(key36);
				// fctiwz to the frame slot [r1+80] and its low word [r1+84] back: the low 32 bits, sign-extended.
				ll_f9.s64 = rex::ppc::fctiwz(ll_f9.f64);
				r11.s64 = int32_t(uint32_t(ll_f9.u64));
				ll_f9.s64 = rex::ppc::fctiwz(ll_f8.f64);
				ll_r10.s64 = int32_t(uint32_t(ll_f9.u64));
				ll_f9.s64 = rex::ppc::fctiwz(ll_f7.f64);
				ll_r9.s64 = int32_t(uint32_t(ll_f9.u64));
				ll_f9.s64 = rex::ppc::fctiwz(ll_f5.f64);
				slot80 = ll_f9.u64;
				ll_r8.s64 = int32_t(uint32_t(ll_f9.u64));
				// The integers through the frame slots [r1+112], [r1+88], [r1+104], [r1+96] and back: the same bits.
				ll_f5.u64 = ll_r10.u64;
				slot88 = r11.u64;
				ll_f9.u64 = r11.u64;
				ll_f7.u64 = ll_r8.u64;
				ll_f9.f64 = double(ll_f9.s64);
				ll_f7.f64 = double(ll_f7.s64);
				ll_f8.u64 = ll_r9.u64;
				ll_f8.f64 = double(ll_f8.s64);
				r11.u64 = ll_r9.u64;
				ll_f9.f64 = double(float(ll_f9.f64));
				ll_f2.f64 = double(float(ll_f7.f64));
				ll_f5.f64 = double(ll_f5.s64);
				ll_f8.f64 = double(float(ll_f8.f64));
				ll_f7.f64 = REX_FMULS(ll_f9.f64, ll_f4.f64);
				ll_f4.f64 = REX_FMULS(ll_f2.f64, ll_f12.f64);
				ll_f5.f64 = double(float(ll_f5.f64));
				ll_f8.f64 = REX_FMULS(ll_f8.f64, ll_f12.f64);
				ll_f9.f64 = REX_FMULS(ll_f5.f64, ll_f10.f64);
				// The copy's four compares (cr6.eq of fcmpu: equal, NaN never), each loading its key component first.
				if (ll_f4.f64 == ll_f3.f64) {
					ll_f5.f64 = double(key40);
					if (ll_f9.f64 == ll_f5.f64) {
						ll_f9.f64 = double(key44);
						if (ll_f8.f64 == ll_f9.f64) {
							ll_f9.f64 = double(key48);
							if (ll_f7.f64 == ll_f9.f64) {
								temp.f32 = float(ll_f6.f64);
								REX_STORE_D_U32_C(r27.u32, 0, temp.u32);
								temp.f32 = float(ll_f6.f64);
								REX_STORE_D_U32_C(r27.u32, 4, temp.u32);
								temp.f32 = float(ll_f6.f64);
								REX_STORE_D_U32_C(r27.u32, 8, temp.u32);
								temp.f32 = float(ll_f6.f64);
								REX_STORE_D_U32_C(r27.u32, 12, temp.u32);
							}
						}
					}
				}
				r27.s64 = r27.s64 + 16;
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
			// The frame slots as the copy's last texel leaves them.
			REX_STORE_D_U64(ll_r1.u32, 80, slot80);
			REX_STORE_D_U64(ll_r1.u32, 88, slot88);
			REX_STORE_D_U64(ll_r1.u32, 96, ll_r9.u64);
			REX_STORE_D_U64(ll_r1.u32, 104, ll_r8.u64);
			REX_STORE_D_U64(ll_r1.u32, 112, ll_r10.u64);
			if (!key_check) [[likely]] goto loc_8282456C_done;
			// Verify mode: keep the fast loop's result, put the inputs back, and run the copy's loop to compare.
			SA2_KEY_REGS(key_check->out, =);
			for (int k = 0; k < 2; ++k) {
				Save(base, key_check->at[k], key_check->len[k], key_check->fast[k]);
				Restore(base, key_check->at[k], key_check->before[k]);
			}
			SA2_KEY_REGS_BACK(key_check->in);
		}
	}
'''
key_tail = r'''	if (key_check) [[unlikely]] {
		uint64_t want[kKeyRegs];
		SA2_KEY_REGS(want, =);
		bool same = std::equal(want, want + kKeyRegs, key_check->out);
		std::vector<uint8_t> now;
		for (int k = 0; k < 2; ++k) {
			Save(base, key_check->at[k], key_check->len[k], now);
			same = same && now == key_check->fast[k];
		}
		::sa2::enh::VerifyCheck();
		static std::atomic<uint32_t> checks_{0};
		const uint32_t count_ = checks_.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count_ == 1 || count_ == 4096 || count_ == 262144)
			REXLOG_INFO("[hot] sub_828243A0 fast colour key: {} checks against the copy's loop", count_);
		if (!same) {
			::sa2::enh::VerifyDifference();
			REXLOG_ERROR("[hot] DIFFERENCE: sub_828243A0's fast colour key ({} texels at {:08X}) differs from the copy's "
			             "loop; the copy's result stands", key_check->len[0] / 16, key_check->at[0]);
		}
		delete key_check;
		key_check = nullptr;
	}
loc_8282456C_done:
'''

ci = src.index(copy_expect)
cj = ci + len(copy_expect)
body = src[:ci] + copy_fast + copy_expect + copy_tail + src[cj:ki] + key_fast + key_expect + key_tail + src[kj:]
decl = '\tuint32_t ea{};\n'
assert body.count(decl) == 1
body = body.replace(decl, decl + '\tSpanCheck* copy_check = nullptr;  // verify mode only\n\tSpanCheck* key_check = nullptr;   // verify mode only\n')
assert body.count('namespace sa2::hot::n_828243A0') == 2
body = body.replace('namespace sa2::hot::n_828243A0 {\n', r'''namespace sa2::hot::n_texel_span {

// Verify mode: the guest bytes a fast loop writes and the registers either loop writes, saved to run the copy's loop
// on the same inputs and compare.
struct SpanCheck {
  uint32_t at[2], len[2];
  std::vector<uint8_t> before[2], fast[2];
  uint64_t in[16], out[16];
};
// (Named one by one: an array of their addresses would make them address-taken, kept in memory in the fast loops too.)
inline constexpr int kCopyRegs = 7;  // compared: the first 7 (f0, fb0, ea are the copy's dead locals after it)
#define SA2_COPY_REGS(a, op)                                                                                   \
  do {                                                                                                         \
    (a)[0] op ll_r6.u64; (a)[1] op ll_r7.u64; (a)[2] op ll_r9.u64; (a)[3] op ll_r10.u64; (a)[4] op r27.u64;    \
    (a)[5] op r11.u64; (a)[6] op ctr.u64; (a)[7] op f0.u64; (a)[8] op fb0.u64; (a)[9] op uint64_t(ea);        \
  } while (0)
#define SA2_COPY_REGS_BACK(a)                                                                                  \
  do {                                                                                                         \
    ll_r6.u64 = (a)[0]; ll_r7.u64 = (a)[1]; ll_r9.u64 = (a)[2]; ll_r10.u64 = (a)[3]; r27.u64 = (a)[4];         \
    r11.u64 = (a)[5]; ctr.u64 = (a)[6]; f0.u64 = (a)[7]; fb0.u64 = (a)[8]; ea = uint32_t((a)[9]);             \
  } while (0)
inline constexpr int kKeyRegs = 13;  // compared: the first 13 (temp is the copy's dead local after it)
#define SA2_KEY_REGS(a, op)                                                                                    \
  do {                                                                                                         \
    (a)[0] op ll_f2.u64; (a)[1] op ll_f3.u64; (a)[2] op ll_f4.u64; (a)[3] op ll_f5.u64; (a)[4] op ll_f7.u64;   \
    (a)[5] op ll_f8.u64; (a)[6] op ll_f9.u64; (a)[7] op ll_r8.u64; (a)[8] op ll_r9.u64; (a)[9] op ll_r10.u64;  \
    (a)[10] op r27.u64; (a)[11] op r11.u64; (a)[12] op ctr.u64; (a)[13] op temp.u64;                          \
  } while (0)
#define SA2_KEY_REGS_BACK(a)                                                                                   \
  do {                                                                                                         \
    ll_f2.u64 = (a)[0]; ll_f3.u64 = (a)[1]; ll_f4.u64 = (a)[2]; ll_f5.u64 = (a)[3]; ll_f7.u64 = (a)[4];        \
    ll_f8.u64 = (a)[5]; ll_f9.u64 = (a)[6]; ll_r8.u64 = (a)[7]; ll_r9.u64 = (a)[8]; ll_r10.u64 = (a)[9];       \
    r27.u64 = (a)[10]; r11.u64 = (a)[11]; ctr.u64 = (a)[12]; temp.u64 = (a)[13];                              \
  } while (0)
inline void Save(const uint8_t* base, uint32_t a, uint32_t len, std::vector<uint8_t>& out) {
  out.resize(len);
  for (uint32_t k = 0; k < len; ++k) out[k] = *Raw(base, a + k);
}
inline void Restore(uint8_t* base, uint32_t a, const std::vector<uint8_t>& in) {
  for (uint32_t k = 0; k < in.size(); ++k) *Raw(base, a + k) = in[k];
}

''', 1)
body = body.replace('}  // namespace sa2::hot::n_828243A0',
                    '#undef SA2_COPY_REGS\n#undef SA2_COPY_REGS_BACK\n#undef SA2_KEY_REGS\n#undef SA2_KEY_REGS_BACK\n\n'
                    '}  // namespace sa2::hot::n_texel_span')
assert 'n_828243A0' not in body.split('#pragma once', 1)[1]

start = body.index('#pragma once')
pragma = '#pragma once\n'
body = body[:start] + pragma + ('\n#include <algorithm>\n#include <atomic>\n#include <cmath>\n#include <vector>\n\n#include <rex/logging.h>\n\n'
                                '#include "../sa2_enhancements.h"\n#include "../sa2_hot_common.h"\n') + body[start + len(pragma):]
header = r'''// sa2-nx (2026-10-28): hand-written sub_828243A0, the third hottest guest function of the game thread in console
// gameplay (5.2 % of its samples, round-6 renut profile): a span of texels of a compressed texture as RGBA floats. It
// decodes the span's row of blocks into the texture's block cache [r31+240] when the cache holds another row (the
// decoder through a pointer: sub_82833568 for DXT1, sub_82834538), copies the span's texels from the cache to r27 (16
// bytes each, swizzled out of the 4x4 blocks), then, when the texture has a colour key ([r31+24]), clears every texel
// that quantises to the key (RGB 5:6:5 and alpha). Made from its localized copy (app/generated/hot/n_828243A0.h),
// which stays its reference: re-run tools/hand_natives/make_texel_span.py after the copy changes (it checks the two
// loops it replaces statement by statement).
//
// Optimisations: (1) the texel copy reloads the cache pointer [r31+240] and marks four cards a texel; the fast copy
// holds the pointer and marks the span's cards once. (2) The colour key reloads six fields of the texture ([r31+128],
// [r31+132], [r31+36..48]) at every texel and moves eight integers through the frame (four fctiwz results through
// [r1+80]/[r1+84], four integer-to-double moves through [r1+88..119]): stores and byte-swapped loads that also keep
// GCC from holding anything across them. The fast loop loads the fields once, converts in registers, and writes the
// frame slots of the last texel once.
//
// Audit (copy): it applies when the span [r27, r27 + 16n) neither wraps at 4 GB nor overlaps [r31+240]: then the
// copy's reloads read the pointer the fast loop holds. Every statement is the copy's, the four loads and stores of a
// texel in its order (a span overlapping the cache reads what the copy reads); f0 / fb0 / temp are the copy's dead
// locals (f0 is set again before the colour key reads it); ctr ends as the copy's after n decrements. The cards (one
// per store: the page of its first byte; the stores tile the span four bytes apart) are the same pages, zeroed after
// the data as the copy zeroes each after its store.
// Audit (colour key): it applies when the span neither wraps nor overlaps the fields [r31+36, r31+52), [r31+128,
// r31+136) or the frame slots [r1+80, r1+120): then the fields read the same at every texel and the slots hold what
// the copy stored last. A slot written and read back is the value written (big-endian: the word at +84 of the double
// word at +80 is its low half, sign-extended by the copy's int32_t), so the integers and the fctiwz results are the
// copy's. Every other statement is the copy's, in its order, including the compares (fcmpu's eq: false with a NaN) and
// the conditional component loads (here of the held values) and the key's four stores with their cards. The registers
// it leaves (f2-f5, f7-f9, r8-r10, r27, r11, ctr) are the copy's last texel's; REX_FLUSH_KNOWN_OFF runs once (nothing
// in the loop changes the mode). Otherwise the copy's loop runs.
//
// Unguarded like its copy (it calls the decoders and sub_8281A688); in verify mode (sa2_cpu_verify) each fast loop
// checks itself: every span it takes is run by both loops on the same inputs (the written bytes and registers saved
// and put back), the copy's result kept, and any difference counted in the [verify] line and logged as
// "[hot] DIFFERENCE".
'''
open(ROOT + 'app/src/cpu/hot/n_texel_span.h', 'w', encoding='utf-8', newline='\n').write(header + body[start:])
print('n_texel_span.h written')
