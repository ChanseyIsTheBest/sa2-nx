"""Writes six hand natives of the third batch (sa2-nx, 2026-10-28, the user's 2P console run) from their localized
copies (app/generated/hot), which stay their references: the render-state handlers and the matrix and colour helpers
of the object walk, with their stores' card marks made once for the record they write (one or two page marks instead
of one a store), and handler 8 calling its body directly.

  n_record_store.h      sub_8214B320 (render-state handler 0): one 20-byte record, five stores
  n_state_bytes.h       sub_8214B168 (handler 5): four byte stores of one word
  n_state_block.h       sub_8214D6D8 (handler 8's body): the seven words of one block before its call
  n_state_handler8.h    sub_8214B3A0 (handler 8, the game thread's hottest function in the 2P profile): its unpack and
                        its body (n_state_block) inline, not through the body's hook
  n_matrix_transpose.h  sub_82147340 (3x4 transpose): twelve stores into one 48-byte matrix
  n_colour_pack.h       sub_8214D9F8 (colour clamp and pack): four float stores and four byte stores

Card marks: a guest store marks the 4 KB page of its first byte (tools/store_cards.py, REX_CARD); the render thread
re-hashes a copy whose pages were marked (render/draw.cpp CardsProve). Marking a range's first and last page once,
after its stores and before the function returns or calls, marks every page the stores marked (a superset where a
store crosses a page: only a re-hash more), with nothing in between that reads the cards. Everything else is the copy's
text. Each replacement checks the copy's lines it replaces (an AssertionError when a codegen changed them).

  python tools/hand_natives/make_card_batches.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl  # noqa: E402

CARDS_ON = '#if !defined(REX_STORE_CARDS_OFF)\n'
CARDS_END = '#endif\n'


def card(expr, n):
    return f'{CARDS_ON}\t::sa2::hot::Card(base, {expr}, {n});\n{CARDS_END}'


def sub(src, old, new, count=1):
    assert src.count(old) == count, f'expected {count} of:\n{old}\nfound {src.count(old)}'
    return src.replace(old, new)


def header(text):
    return '// sa2-nx (2026-10-28): ' + text.strip().replace('\n', '\n// ') + '\n//\n'


# sub_8214B320: the record [r10 + 0x83853480, +17): four words and a byte.
src = fl.copy_text('8214B320')
old = ('\tREX_STORE_U32_C(l_r10.u32 + l_r6.u32, l_r8.u32);\n'
       '\tREX_STORE_U32_C(l_r10.u32 + l_r5.u32, l_r7.u32);\n'
       '\tREX_STORE_U32_C(l_r10.u32 + r30.u32, l_r4.u32);\n'
       '\tREX_STORE_U32_C(l_r10.u32 + l_r9.u32, r31.u32);\n'
       '\tREX_STORE_U8_C(l_r10.u32 + r11.u32, l_r3.u8);\n')
src = sub(src, old, old.replace('_C(', '(') + card('l_r10.u32 + l_r5.u32', 17))
fl.finish(src, '8214B320', 'n_record_store', header('''
hand-written sub_8214B320 (render-state handler 0): fills one 20-byte record of a table at 0x83853480 from three
lookups indexed by r3. Made from its localized copy (app/generated/hot/n_8214B320.h, its reference) by
tools/hand_natives/make_card_batches.py: the five stores' card marks as one mark of the record (r5 = 0x83853480,
the record at r10 + r5, 17 bytes from its first store to its byte). Guarded as its copy was.'''), 'n_record_store.h')

# sub_8214B168: four bytes at 0x8389F780 (r11 + 96 .. 99).
src = fl.copy_text('8214B168')
old = ('\tREX_STORE_D_U8_C(r11.u32, 96, l_r10.u8);\n'
       '\tREX_STORE_D_U8_C(r11.u32, 97, l_r9.u8);\n'
       '\tcr6.compare<uint32_t>(l_r5.u32, 0, xer);\n'
       '\tREX_STORE_D_U8_C(r11.u32, 98, l_r8.u8);\n'
       '\tREX_STORE_D_U8_C(r11.u32, 99, l_r7.u8);\n')
src = sub(src, old, old.replace('_C(', '(') + card('r11.u32 + 96', 4))
fl.finish(src, '8214B168', 'n_state_bytes', header('''
hand-written sub_8214B168 (render-state handler 5): stores r3's four bytes into the render-state block and sets
the colour from them. Made from its localized copy (app/generated/hot/n_8214B168.h, its reference) by
tools/hand_natives/make_card_batches.py: the four byte stores' card marks as one mark of their word (r11 + 96, before
anything that follows reads or calls). Unguarded as its copy (it calls).'''), 'n_state_bytes.h')

# sub_8214D6D8: the block at r3 = 0x8389F8A8, [r3, +28), on both paths, before its call.
src = fl.copy_text('8214D6D8')
for off in (0, 12, 16, 24):
    src = sub(src, f'\tREX_STORE_D_U32_C(l_r3.u32, {off}, ', f'\tREX_STORE_D_U32(l_r3.u32, {off}, ', 2)
for off in (20, 4, 8):
    src = sub(src, f'\tREX_STORE_D_U32_C(l_r3.u32, {off}, ', f'\tREX_STORE_D_U32(l_r3.u32, {off}, ')
call = '\t{ do { ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); sub_8213D1F8(ctx, base); SA2_SYNC_IN(); }\n'
src = sub(src, call, card('l_r3.u32', 28) + call)
assert 'REX_STORE_D_U32_C(l_r8.u32, 2080, ' in src  # the one store elsewhere keeps its own mark
fl.finish(src, '8214D6D8', 'n_state_block', header('''
hand-written sub_8214D6D8 (render-state handler 8's body): fills the seven words of a block at 0x8389F8A8 and calls
sub_8213D1F8. Made from its localized copy (app/generated/hot/n_8214D6D8.h, its reference) by
tools/hand_natives/make_card_batches.py: the block's stores' card marks (four or five on each path, then three) as
one mark of [r3, +28) before the call; the store at +2080 of another block keeps its own. Unguarded as its copy.'''),
          'n_state_block.h')

# sub_8214B3A0: its unpack, then its body directly (the body's hook only dispatched to the same copy).
src = fl.copy_text('8214B3A0')
tail = 'sub_8214D6D8(ctx, base);'
assert src.count(tail) == 1, 'the tail call'
src = src.replace(tail, '::sa2::hot::n_state_block::Native(ctx, base);')
src = sub(src, 'REX_EXTERN(sub_8214D6D8);\n', '')
src = src.replace('#pragma once\n', '#pragma once\n\n#include "n_state_block.h"\n', 1)
fl.finish(src, '8214B3A0', 'n_state_handler8', header('''
hand-written sub_8214B3A0 (render-state handler 8, the game thread's hottest function in the 2P console profile):
splits r3 into its four fields and branches to sub_8214D6D8. Made from its localized copy
(app/generated/hot/n_8214B3A0.h, its reference) by tools/hand_natives/make_card_batches.py: the body runs as
n_state_block (hot/n_state_block.h) inline, not through sub_8214D6D8's hook, which only dispatched to the copy it was
made from. Unguarded as its copy.'''), 'n_state_handler8.h')

# sub_82147340: the 48-byte matrix at r4 (both paths end at loc_821473CC's three stores).
src = fl.copy_text('82147340')
n = src.count('REX_STORE_D_U32_C(l_r4.u32, ')
assert n == 18, n  # 6 in place or 9 copied, then 3 (12, 28, 44)
src = src.replace('REX_STORE_D_U32_C(l_r4.u32, ', 'REX_STORE_D_U32(l_r4.u32, ')
ret = '\t{ do { ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; } while (0); return; }\n'
src = sub(src, ret, card('l_r4.u32', 48) + ret)
fl.finish(src, '82147340', 'n_matrix_transpose', header('''
hand-written sub_82147340: transposes the 3x3 part of a 3x4 matrix (r3 into r4, or r4 in place) and zeroes its
translation. Made from its localized copy (app/generated/hot/n_82147340.h, its reference) by
tools/hand_natives/make_card_batches.py: the stores' card marks (nine or twelve) as one mark of [r4, +48) before its return
(the r4 = 0 return stores nothing). Guarded.'''), 'n_matrix_transpose.h')

# sub_8214D9F8: [r3, +16) and [r4, +4).
src = fl.copy_text('8214D9F8')
for off in (0, 4, 8, 12):
    src = sub(src, f'REX_STORE_D_U32_C(l_r3.u32, {off}, ', f'REX_STORE_D_U32(l_r3.u32, {off}, ')
for off in (3, 2, 0, 1):
    src = sub(src, f'REX_STORE_D_U8_C(l_r4.u32, {off}, ', f'REX_STORE_D_U8(l_r4.u32, {off}, ')
assert '_C(' not in src.split('inline void Native', 1)[1]
ret = '\t{ do { ctx.f3 = l_f3; ctx.f4 = l_f4; ctx.f5 = l_f5; ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); return; }\n'
src = sub(src, ret, card('l_r3.u32', 16) + card('l_r4.u32', 4) + ret)
fl.finish(src, '8214D9F8', 'n_colour_pack', header('''
hand-written sub_8214D9F8: clamps the four floats at r3 to [0, 1] (stored back) and packs them as bytes at r4.
Made from its localized copy (app/generated/hot/n_8214D9F8.h, its reference) by
tools/hand_natives/make_card_batches.py: the four float stores' marks as one of [r3, +16) and the four byte stores'
as one of [r4, +4), before its return. Guarded.'''), 'n_colour_pack.h')
