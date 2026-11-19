#!/usr/bin/env python3
"""banjo-nx (2026-10-19, CPU proposal 1 of docs/cpu-codegen-proposals-2026-10-19.md): the store cards.

Renut keeps converted copies of the game's vertex and index data and, with banjo_renut_exact_streams, proves each copy
by hashing its guest bytes again at its first use after every kick (16 MB a frame in gameplay, a quarter of the render
thread on the console). With the cards it hashes only data whose memory was written since the copy was proven: every
guest store outside the stack also zeroes its 4 KB page's card, the byte at guest address ea >> 12 (the first MB of the
guest window, reserved for them in Memory::Initialize), and Renut sets the cards of the pages it checks back to 1.

This step renames the stores that may write outside the stack to card-marking forms:
    REX_STORE_D_U32(r31.u32, 8, x);         ->  REX_STORE_D_U32_C(r31.u32, 8, x);
    REX_STORE_U32(ea, x);                   ->  REX_STORE_U32_C(ea, x);
    simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), ...);   ->  REX_CARD(ea), simde_mm_store_si128(...);
    memset((void*)REX_RAW_ADDR(ea), 0, 128);                      ->  REX_CARD(ea), memset(...);   (dcbz, dcbzl)
    REX_GUEST_CAS(reinterpret_cast<...>(REX_RAW_ADDR(ea)), ...)   ->  REX_GUEST_CAS_C(ea, reinterpret_cast<...>...)
    REX_STVLX(ea, v); REX_STVRX(ea, v); REX_MM_STORE_U32(a, v);    ->  the _C forms
Stores through the guest stack pointer (r1 or its local mirrors as the base, or an ea computed from it) are left as
they are: the stack is never vertex or index data. A store marks the card of its first byte's page: a vector store,
dcbz, stvlx and stvrx stay in one aligned block, but an unaligned store of 2-8 bytes may run into the next page, which
it does not mark, so Renut counts a page as written when its card or the card of the page before it is (one card
store per guest store instead of two).

The inline memcpy / memset of tools/inline_memcalls.py need no cards: they never write the physical views
(rex::ppc::PlainRange), where all GPU data lives; the CRT hooks that do (sdk/src/kernel/crt/memory.cpp) mark their
ranges themselves, as do the other host writers of guest memory.

REX_STORE_CARDS_OFF (compile time) makes every card a no-op. Idempotent; --restore puts the plain forms back.
  python tools/store_cards.py --gen app/generated [--restore] [--check]
"""
import argparse
import glob
import os
import re
import sys

R1 = re.compile(r'(?<![\w.])(?:ctx\.r1|r1l|l_r1|ll_r1|r1)\.[us](?:32|64)\b')
# blur-nx (2026-11-13, M-23): and tools/float_loads.py's single stores from the vector unit (REX_STORE_(D_)F32ANY).
STORE = re.compile(r'\b(REX_STORE_D_U(?:8|16|32|64)|REX_STORE_U(?:8|16|32|64)|REX_STVLX|REX_STVRX|'
                   r'REX_MM_STORE_U(?:8|16|32|64)|BANJO_HOT_STORE_(?:D_)?U(?:8|16|32|64)|'
                   r'REX_STORE_(?:D_)?F32ANY|BANJO_HOT_STORE_(?:D_)?F32ANY)\(')
VEC = 'simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),'
DCBZ = re.compile(r'(?<![\w,] )\bmemset\(\(void\*\)REX_RAW_ADDR\(ea\), 0, (?:32|128)\);')
CAS = 'REX_GUEST_CAS(reinterpret_cast<'
EA_SET = re.compile(r'^\s*ea = (.*);\s*$')
INSN = re.compile(r'^\s*// ')

MARKER = '// banjo-nx: the store cards (tools/store_cards.py)'
END = '// (end of the macros of tools/store_cards.py)\n'
MACROS = MARKER + '''
// A guest store outside the stack also zeroes its 4 KB page's card: the byte at guest address ea >> 12, in the first
// MB of the guest window (reserved for them). Renut proves its cached vertex and index copies by their pages' cards
// instead of hashing them at every kick (banjo_renut_store_cards). A store marks its first byte's page; one that runs
// into the next page (unaligned) does not mark that one, so Renut reads a page's card together with the page before's.
#ifndef REX_CARD
#if defined(REX_STORE_CARDS_OFF)
#define REX_CARD(ea) ((void)0)
#elif defined(REX_NX_CENSUS_ON)  // blur-nx (2026-11-13, near-exact #38's census: card marks of the generated tree)
#define REX_CARD(ea) ((void)(REX_NX_CENSUS(25), *(u8*)(base + ((u32)(ea) >> 12)) = 0))
#elif defined(REX_CARD_VIEWS_ONLY)
// blur-nx (2026-11-13, with near-exact #38; exact): the copies' rule (cpu/banjo_hot_prelude.h, VS-11 / VS-12 of
// 2026-11-11) for the generated tree too: only the pages whose cards Renut reads (its views 0xA0000000 and up and
// 0x7F000000-0x7FFFFFFF, and the page in front of each), and the card byte written only when it is not zero yet (a
// zero card is the result either way; the card lines stay shared between the cores).
[[gnu::always_inline]] static inline void rex_card_gen_(uint8_t* base_, u32 page) {
  if (page - 0x9FFFFu <= 0x60000u || page - 0x7EFFFu <= 0x1000u) {
    u8* const card = (u8*)(base_ + page);
    if (*card) *card = 0;
  }
}
#define REX_CARD(ea) rex_card_gen_(base, ((u32)(ea)) >> 12)
#else
#define REX_CARD(ea) ((void)(*(u8*)(base + ((u32)(ea) >> 12)) = 0))
#endif
#if defined(REX_CARD_ADDR_TWICE)
// The forms before port 160 (A/B only): the address written twice.
#define REX_STORE_D_U8_C(r, d, y) (REX_STORE_D_U8(r, d, y), REX_CARD((u32)(r) + (u32)(d)))
#define REX_STORE_D_U16_C(r, d, y) (REX_STORE_D_U16(r, d, y), REX_CARD((u32)(r) + (u32)(d)))
#define REX_STORE_D_U32_C(r, d, y) (REX_STORE_D_U32(r, d, y), REX_CARD((u32)(r) + (u32)(d)))
#define REX_STORE_D_U64_C(r, d, y) (REX_STORE_D_U64(r, d, y), REX_CARD((u32)(r) + (u32)(d)))
#define REX_STORE_U8_C(x, y) (REX_STORE_U8(x, y), REX_CARD(x))
#define REX_STORE_U16_C(x, y) (REX_STORE_U16(x, y), REX_CARD(x))
#define REX_STORE_U32_C(x, y) (REX_STORE_U32(x, y), REX_CARD(x))
#define REX_STORE_U64_C(x, y) (REX_STORE_U64(x, y), REX_CARD(x))
#define REX_MM_STORE_U8_C(a, v) do { REX_MM_STORE_U8(a, v); REX_CARD(a); } while (0)
#define REX_MM_STORE_U16_C(a, v) do { REX_MM_STORE_U16(a, v); REX_CARD(a); } while (0)
#define REX_MM_STORE_U32_C(a, v) do { REX_MM_STORE_U32(a, v); REX_CARD(a); } while (0)
#define REX_MM_STORE_U64_C(a, v) do { REX_MM_STORE_U64(a, v); REX_CARD(a); } while (0)
#else
// Port 160 (from fnxf_xiii, its note of 2026-10-24): each form evaluates its address once. The stores are volatile
// and, with -fno-strict-aliasing, may change ctx as far as the compiler knows, so an address written twice was loaded
// from ctx and added again for the card (fnxf_xiii: 2.3 MB of .text, about 250k extra ldr / add). The same address
// and the same card; -DREX_CARD_ADDR_TWICE gives the old forms back.
#define REX_STORE_D_U8_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U8(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_D_U16_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U16(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_D_U32_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U32(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_D_U64_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U64(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_U8_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U8(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_STORE_U16_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U16(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_STORE_U32_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U32(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_STORE_U64_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U64(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_MM_STORE_U8_C(a, v) \
  do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U8(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#define REX_MM_STORE_U16_C(a, v) \
  do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U16(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#define REX_MM_STORE_U32_C(a, v) \
  do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U32(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#define REX_MM_STORE_U64_C(a, v) \
  do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U64(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#endif
#define REX_STVLX_C(ea, v) do { REX_STVLX(ea, v); REX_CARD(ea); } while (0)
#define REX_STVRX_C(ea, v) do { REX_STVRX(ea, v); REX_CARD(ea); } while (0)
#define REX_GUEST_CAS_C(ea, p, e, d) (REX_CARD(ea), REX_GUEST_CAS(p, e, d))
#endif  // REX_CARD
''' + END


def first_arg(text, start):
    """The first argument of the call whose '(' is at text[start - 1]: up to the first top-level comma."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c in '([':
            depth += 1
        elif c in ')]':
            if depth == 0:
                return text[start:i]
            depth -= 1
        elif c == ',' and depth == 0:
            return text[start:i]
        i += 1
    return text[start:]


def stack_ea(lines, k):
    """Whether the ea a store on lines[k] uses was computed from r1 by its own instruction (the lines since the
    instruction's comment), or from one register an earlier line of the same straight line set to r1 plus a constant
    (sasrt-nx 2026-11-11, port 74; its idea 31: `r29 = r1 - 64` ... `ea = (r29.u32) & ~0xF`). Not found: not known to
    be the stack."""
    j = k
    while j >= 0:
        m = EA_SET.match(lines[j])
        if m:
            return bool(R1.search(m.group(1))) or (STACK_DERIVED and derived_from_r1(lines, j, m.group(1)))
        if j < k and INSN.match(lines[j]):
            return False
        j -= 1
    return False


# sasrt-nx (2026-11-11, port 74; its idea 31): 10 % of sasrt-nx's stack stores in copies went through a register set to
# r1 plus a constant and kept their cards. The scan back stops at a label, a call, a brace and at the macros that write
# registers (REX_LOOP_IN / OUT, REX_LR_READ: sasrt-nx's batch 3 audit, finding 3), and at any other write of the
# register. Only stores through `ea` (the D-form stores keep the plain r1 test). SASRT_CARDS_STACK_DERIVED=0: as before.
STACK_DERIVED = os.environ.get('SASRT_CARDS_STACK_DERIVED', '1') != '0'
REG_USE = re.compile(r'(?<![\w.])((?:ctx\.)?(?:l{1,2}_)?r\d+)\.(?:u32|s32|u64|s64)\b')
STRAIGHT_END = re.compile(r'^loc_|\(ctx, base|REX_CALL|REX_LOOP_|REX_LR_|[{}]')
R1_PLUS = re.compile(r'\s*(?:ctx\.)?(?:l{1,2}_)?r1\.(?:s64|u64|u32|s32) \+ -?\d+\s*')


def derived_from_r1(lines, j, expr):
    """Whether expr's one register (not r1 itself) was last set, on the same straight line before lines[j], to r1 plus
    a constant."""
    regs = set(REG_USE.findall(expr))
    if len(regs) != 1:
        return False
    reg = regs.pop()
    define = re.compile(r'^\t+' + re.escape(reg) + r'\.(?:u32|s32|u64|s64) = (.*);$')
    other_write = re.compile(r'(?<![\w.])' + re.escape(reg) + r'(?:\.\w+)?\s*=[^=]')
    for q in range(j - 1, max(-1, j - 400), -1):
        line = lines[q]
        if STRAIGHT_END.search(line):
            return False
        d = define.match(line)
        if d:
            return R1_PLUS.fullmatch(d.group(1)) is not None
        if other_write.search(line):
            return False  # written some other way: not known
    return False


def stack_arg(arg, lines, k):
    arg = arg.strip()
    if arg == 'ea' or arg.startswith('ea ') or arg.startswith('(ea'):
        return stack_ea(lines, k)
    return bool(R1.search(arg))


def apply_line(lines, k, counts):
    line = lines[k]
    if INSN.match(line):
        return line
    out = []
    pos = 0
    for m in STORE.finditer(line):
        name = m.group(1)
        arg = first_arg(line, m.end())
        if stack_arg(arg, lines, k):
            counts['stack'] += 1
            continue
        out.append(line[pos:m.start()])
        out.append(name + '_C(')
        pos = m.end()
        key = ('BANJO_HOT' if name.startswith('BANJO_HOT') else
               'REX_STORE_D' if name == 'REX_STORE_D_F32ANY' else 'REX_STORE' if name == 'REX_STORE_F32ANY' else
               (name.split('_U')[0] if '_U' in name else name))
        counts[key] += 1
    out.append(line[pos:])
    line = ''.join(out)
    if VEC in line and 'REX_CARD(ea), ' + VEC not in line:
        if stack_ea(lines, k):
            counts['stack'] += 1
        else:
            line = line.replace(VEC, 'REX_CARD(ea), ' + VEC)
            counts['vector'] += 1
    m = DCBZ.search(line)
    if m and 'REX_CARD(ea), memset' not in line:
        if stack_ea(lines, k):
            counts['stack'] += 1
        else:
            line = line[:m.start()] + 'REX_CARD(ea), ' + line[m.start():]
            counts['dcbz'] += 1
    if CAS in line and 'REX_RAW_ADDR(ea)' in line:
        if stack_ea(lines, k):
            counts['stack'] += 1
        else:
            line = line.replace(CAS, 'REX_GUEST_CAS_C(ea, reinterpret_cast<')
            counts['cas'] += 1
    return line


UNDO = [
    (re.compile(r'\b(REX_STORE_D_U(?:8|16|32|64)|REX_STORE_U(?:8|16|32|64)|REX_STVLX|REX_STVRX|'
                r'REX_MM_STORE_U(?:8|16|32|64)|BANJO_HOT_STORE_(?:D_)?U(?:8|16|32|64)|'
                r'REX_STORE_(?:D_)?F32ANY|BANJO_HOT_STORE_(?:D_)?F32ANY)_C\('), r'\1('),
    (re.compile(r'REX_CARD\(ea\), (?=simde_mm_store_si128|memset\()'), ''),
    (re.compile(r'REX_GUEST_CAS_C\(ea, '), 'REX_GUEST_CAS('),
]


def restore_text(text):
    for rx, rep in UNDO:
        text = rx.sub(rep, text)
    return text


def files(gen):
    return (sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))) +
            sorted(glob.glob(os.path.join(gen, 'hot', 'n_*.h'))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp, hot/ and *_pch.h)')
    ap.add_argument('--restore', action='store_true', help='put the plain stores back (and drop the macros)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    counts = {k: 0 for k in ('REX_STORE_D', 'REX_STORE', 'REX_STVLX', 'REX_STVRX', 'REX_MM_STORE', 'BANJO_HOT',
                             'vector', 'dcbz', 'cas', 'stack')}
    changed = 0
    for path in files(a.gen):
        text = open(path, encoding='utf-8').read()
        base = restore_text(text)
        if a.restore:
            new = base
        else:
            lines = base.split('\n')
            new = '\n'.join(apply_line(lines, k, counts) for k in range(len(lines)))
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): store_cards: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    new = text
    if MARKER in new:
        start = new.index(MARKER)
        new = new[:start] + new[new.index(END, start) + len(END):]
    if not a.restore:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in new:
            sys.exit('ERROR (post-codegen patch): store_cards: no REX_LOAD_U8 in %s' % pchs[0])
        new = new.replace(anchor, MACROS + anchor, 1)
    if new != text and not a.check:
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(new)
    if a.restore:
        print('store_cards: plain stores restored (%d files)' % changed)
    else:
        print('store_cards%s: cards on %d D-form, %d X-form, %d vector, %d dcbz, %d compare-and-swap, %d stvlx, %d '
              'stvrx, %d MMIO-capable and %d localized-copy stores; %d stack stores left plain (%d files)' %
              (' (check only)' if a.check else '', counts['REX_STORE_D'], counts['REX_STORE'], counts['vector'],
               counts['dcbz'], counts['cas'], counts['REX_STVLX'], counts['REX_STVRX'], counts['REX_MM_STORE'],
               counts['BANJO_HOT'], counts['stack'], changed))


if __name__ == '__main__':
    main()
