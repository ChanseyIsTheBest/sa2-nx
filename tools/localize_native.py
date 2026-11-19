#!/usr/bin/env python3
"""banjo-nx: "localized copies" of recompiled guest functions, the native replacements of app/src/cpu/banjo_hot_guest.cpp.

From sa2-nx's tools/localize_native.py (GPL-3.0, the same project family; sa2-nx took the idea from
UnleashedRecomp-NX's switch-localize.py), rewritten for Banjo's generated code and for a guard that needs no
hand-written list of the memory a function writes.

A copy runs the recompiled function's own statements (app/generated/renut_recomp.N.cpp, after every post-codegen step)
with these differences:
  - the guest registers the function keeps in the PPCContext (the volatile ones: r0, r3-r12, f0-f13, v0-v13, and
    whatever else it mentions as ctx.<register>) live in C++ locals: loaded at entry, written back at every return
    (only those the function can have changed) and synchronised around calls (below);
  - direct callees that are small enough come along as copies of their own, in the same header ("the closure"), so the
    compiler can inline them and a call costs no hook and no full register hand-over;
  - a call synchronises only what the callee can reach: before it, the registers this function may have changed that
    the callee (or anything it calls) mentions; after it, the registers the callee (or anything it calls) can have
    changed. A callee whose reach is unknown (an indirect call, a trap, a kernel import, a function an app hook
    replaces, a mid-asm hook) gets every register.

Each function is written three times into app/generated/hot/n_<id>.h:
  f_<name><Fast>     the copy that runs;
  f_<name><Logged>   the same text, every guest store first recorded in a write log (address, length, old bytes);
  o_<name>           the ORIGINAL statements (registers in ctx), stores recorded the same way.
The two logged forms are how the guard learns which guest memory a call writes without a list per function: it runs
both on copies of the registers, rolls the memory back from the log, and then runs the real copy and the real
recompiled function over a snapshot of exactly those bytes (banjo_hot_guest.cpp). A logged form stops at the first
call it cannot repeat (anything outside the closure): that call of the function is then not comparable.
sasrt-nx (2026-11-11, port 81; machine copies in place): the entry, n_<id>::Native, is
written "BANJO_HOT_NATIVE_SPEC inline void Native" (app/src/cpu/banjo_hot_guest.h: [[gnu::noinline]] behind a hook,
nothing with CMake SASRT_HOT_IN_PLACE, where the hook is a bare redirect and the copy may be inlined into its callers).

What cannot be a copy (the tool says why and writes nothing): a function, or a closure member, with setjmp / longjmp,
an atomic compare-and-swap or a memory barrier (state shared with other threads: the guard's roll-back would be seen),
MMIO, the guest time base, a mid-asm hook, or a guest-memory write in a form the tool does not know.

HAND-WRITTEN NATIVES (app/src/cpu/hot/hand.txt, the code in app/src/cpu/hot/banjo_hand_natives.h). A function of
that list has a native version written by hand, banjo::hand::n_<id>::Native(ctx, base, s), which returns false when it
declines a call. Wherever this tool uses the generated copy f_<name> of such a function (as the function of a hook, or
as a callee inside another function's copy) the hand-written native is asked first and the generated copy runs only
when it declines. The original's logged form o_<name> is untouched: the guard still compares against the original's
statements. A hand-written native writes no register; the registers the original changes (this tool reads them from
its statements; hand.txt's exact= names those the native leaves as the original does) are that hook's "skip" set,
written as kSkip into every header whose copy contains the native: the guard does not compare them.

  python tools/localize_native.py --list app/src/cpu/hot/localized.txt      every function of the list (post_codegen.py)
  python tools/localize_native.py --list --bulk                             and the bulk list (post_codegen.py)

BULK (2026-10-22, bejeweled3-nx's method, docs/enhancements.md "Bulk localized copies"): app/src/cpu/hot/
localized_bulk.txt (written by tools/switch_hot_bulk.py from console profiles) lists hundreds more functions. Each is
made the same way; one the tool refuses is left out and named in the output. Their hooks are BANJO_HOT_HOOK_BULK lines
spread over app/generated/hot/banjo_hot_bulk_<k>.inc by the size of their headers, each compiled by its own
app/src/cpu/banjo_hot_shard_<k>.cpp, so the copies compile in parallel. One master key, banjo_cpu_native_bulk,
switches them all (each also has its own key).
  python tools/localize_native.py --survey sub_82949790 [more ...]         what a copy of each would look like
"""
import argparse
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reg_liveness as reg_liveness_mod  # noqa: E402  (sasrt-nx 2026-11-12, stage 1; port 121: tools/reg_liveness.py)


def gen_file(gen, suffix):
    """renut-ultimate (2026-11-19, port 186; found by sasrt-nx's sync): the generator names its files after the project
    (<project>_pch.h, <project>_init.cpp, <project>_recomp.N.cpp): renut_ in the base, a port's own name otherwise."""
    for name in sorted(os.listdir(gen)):
        m = re.fullmatch(r'(\w+)_recomp\.\d+\.cpp', name)
        if m:
            return os.path.join(gen, m.group(1) + '_' + suffix)
    return os.path.join(gen, 'renut_' + suffix)


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
OUT = os.path.join(GEN, 'hot')
IMAGE = os.path.join(ROOT, 'out', 'renut_image.bin')
INDEX = os.path.join(ROOT, 'out', 'localize_index.json')
LIST = os.path.join(APP, 'src', 'cpu', 'hot', 'localized.txt')
BULK = os.path.join(APP, 'src', 'cpu', 'hot', 'localized_bulk.txt')
SHARDS = 12  # app/src/cpu/banjo_hot_shard_<k>.cpp (app/CMakeLists.txt lists them)
# sasrt-nx (2026-11-11, port 74; its idea 37): a copy writes back at a return only the registers a caller can read:
# tools/leaf_dead_stores.py's LiveOut of the function (whole program, no ABI assumption: the 360 compiler allocates
# registers across functions), plus r1, r2, r13, r3 and f1 and every non-volatile one, as leaf_dead_stores.py's own
# rule. The original writes them; the guard does not compare them (kSkip). SASRT_LOCALIZE_LIVE_OUT=0: every changed
# register written back, as before.
LIVE_OUT = os.environ.get('SASRT_LOCALIZE_LIVE_OUT', '1') != '0'
LIVE_OUT_JSON = os.path.join(ROOT, 'out', 'leaf_live_out.json')
_live_out = None
LIVE_OUT_DROPPED = [0]
# sasrt-nx (2026-11-12, its docs/calling-convention-plan.md stage 1; port 121): tools/reg_liveness.py's whole-program
# liveness with the vector registers: a real call's hand-over leaves out the registers the callee does not read before
# writing them and nothing reads after the call; a return leaves out the vector write-backs no caller reads (and the
# guard does not compare those). SASRT_LOCALIZE_CALL_LIVENESS=0: as before.
CALL_LIVENESS = os.environ.get('SASRT_LOCALIZE_CALL_LIVENESS', '1') != '0'
REG_LIVENESS_JSON = os.path.join(ROOT, 'out', 'reg_liveness.json')
REG_ALL = (1 << 192) - 1
_reg_liveness = None
CALL_SKIPPED = [0, 0, 0, 0]  # (call sites with a store left out, stores left out; the same at indirect calls)


def reg_liveness():
    """{'live_out': {address: mask}, 'sites': {address: {instruction index: (callee address, mask)}}} of this tree."""
    global _reg_liveness
    if _reg_liveness is None:
        _reg_liveness = {'live_out': {}, 'sites': {}, 'indirect': {}}
        if CALL_LIVENESS:
            try:
                data = json.load(open(REG_LIVENESS_JSON))
                if os.path.normcase(os.path.abspath(data.get('gen', ''))) != os.path.normcase(os.path.abspath(GEN)):
                    raise ValueError('another tree')
                if data.get('stamp') != reg_liveness_mod.tree_stamp(GEN):
                    raise ValueError('the tree changed since the analysis')
                _reg_liveness = {
                    'live_out': {int(k, 16): int(v, 16) for k, v in data['live_out'].items()},
                    'sites': {int(k, 16): {row[0]: (int(row[1], 16), int(row[2], 16)) for row in rows}
                              for k, rows in data['sites'].items()},
                    'indirect': {int(k, 16): {row[0]: int(row[1], 16) for row in rows}
                                 for k, rows in data.get('indirect', {}).items()}}
            except (OSError, ValueError, KeyError, AttributeError, TypeError):
                _reg_liveness = {'live_out': {}, 'sites': {}, 'indirect': {}}
    return _reg_liveness


def reg_bit(reg):
    """The bit of a register name in tools/reg_liveness.py's masks (None: not a general, float or vector register)."""
    k = re.match(r'^([rfv])(\d+)$', reg)
    if not k:
        return None
    n = int(k.group(2))
    return n if k.group(1) == 'r' else 32 + n if k.group(1) == 'f' else 64 + n


def live_out_keep(name):
    """None (keep every register) or a predicate on register names for the function's return write-backs."""
    global _live_out
    if not LIVE_OUT:
        return None
    if _live_out is None:
        try:
            data = json.load(open(LIVE_OUT_JSON))
            # (sasrt-nx's round A audit, finding 6: only the analysis of this tree; a scratch run's, or none, trims
            # nothing.)
            if os.path.normcase(os.path.abspath(data.get('gen', ''))) != os.path.normcase(os.path.abspath(GEN)):
                raise ValueError('another tree')
            _live_out = {int(k, 16): v for k, v in data['live_out'].items()}
        except (OSError, ValueError, KeyError, AttributeError):
            _live_out = {}
    m = re.match(r'^(?:__imp__)?sub_([0-9A-F]{8})$', name)
    mask = _live_out.get(int(m.group(1), 16)) if m else None
    # sasrt-nx (2026-11-12, stage 1; port 121): the vector registers' LiveOut (tools/reg_liveness.py)
    vmask = reg_liveness()['live_out'].get(int(m.group(1), 16)) if m else None
    if vmask == REG_ALL:
        vmask = None
    if mask == (1 << 64) - 1:
        mask = None
    if mask is None and vmask is None:
        return None

    def keep(reg):
        if reg.startswith('v') and reg[1:].isdigit():
            return True if vmask is None else bool(vmask >> (64 + int(reg[1:])) & 1)
        k = re.match(r'^([rf])(\d+)$', reg)
        if not k:
            return True
        n = int(k.group(2))
        # sasrt-nx (2026-11-12, stage 1c; port 121): a general / float register is left out when either analysis finds
        # it dead after the return: leaf_dead_stores.py's LiveOut, or tools/reg_liveness.py's, which also bounds a
        # function entered through a pointer by the convention (its per-call-site hand-overs use the same LiveOut, so
        # the two agree; the guard's skip set takes this predicate).
        if k.group(1) == 'r':
            if n in (1, 2, 3, 13) or n > 12:
                return True
            return (mask is None or bool(mask >> n & 1)) and (vmask is None or bool(vmask >> n & 1))
        if n == 1 or n > 13:
            return True
        return (mask is None or bool(mask >> (32 + n) & 1)) and (vmask is None or bool(vmask >> (32 + n) & 1))
    return keep


BULK_SLOT = 9999  # past PPCContext::host_counters: the guard's countdown is the hook's own (banjo_hot_guest.h)
HAND = os.path.join(APP, 'src', 'cpu', 'hot', 'hand.txt')
IMAGE_BASE = 0x82000000
INDEX_FORMAT = 12  # raise when summarise() changes: the cached index is rebuilt (5: carded vector stores; 6, 7: inline leaves; 8: r1 mirror, 2026-10-23; 9: atomics, sasrt-nx 2026-11-10; 10: promoted fallbacks counted, blur-nx 2026-11-13; 12: rex::ppc::exact:: helpers and rex_spin_hint known, port 144)
# blur-nx (2026-11-13, M-25 of docs/cpu-moonshots-50-1113.md and its audit): a promoted indirect call (tools/
# indirect_calls.py: `if (REX_PROMOTED_IS(ctr, T)) { sub_T(ctx, base); } else { REX_CALL_INDIRECT_FUNC_AT(..); }`) no
# longer keeps a function out of a closure: its fallback is an open call like any other (the copy's running form
# hands over every register around it, the logged forms stop there), and its guarded direct call is a call of T,
# which comes along when T's own tree qualifies. Before, every function with such a site counted as making an indirect
# call, and the promoted targets were real calls from the copies: of the copies' 623 M promoted calls in the PC
# census, the targets of 514 M (82.5 %) have trees closed but for promoted fallbacks. A member's reach stays everything
# when it has a fallback (Reach.own): the hand-over around the call is the same; the call, the target's entry and
# return and the context round trip go when it is inlined.
# 2026-11-13, cycle 2's PC guard runs: OFF unless BLUR_LOCALIZE_PROMOTED_MEMBERS=1. With it on, guarded boots hung
# before boot.pak in 6 of 8 runs (all 203 copies it changed switched off at run time: 2 of 2 booted; a bisect was misled
# by the hang being random), and the light guard found copies of promoted targets writing back r12 where the original
# leaves it (sub_82507988, sub_825087A0; harmless by the calling convention, but the guard turns them off). Not
# verifiable as it stands: parked.
PROMOTED_MEMBERS = os.environ.get('BLUR_LOCALIZE_PROMOTED_MEMBERS', '0') == '1'
# blur-nx (2026-11-17, #232): the non-volatile registers of the Xbox 360's PowerPC ABI (r14-r31, f14-f31, the VMX
# v14-v31 and VMX128 v64-v127, CR fields 2-4): a real call returns them as it found them (call_text).
# renut-ultimate (2026-11-17, port 149): OFF in the base, as every near-exact switch is (port 140): a callee that breaks
# the ABI (hand-written assembly) is the one case it changes. BLUR_LOCALIZE_ABI_TRUST=1 in the environment turns it on.
ABI_TRUST = os.environ.get('BLUR_LOCALIZE_ABI_TRUST', '0') == '1'
NONVOLATILE = frozenset(['r%d' % i for i in range(14, 32)] + ['f%d' % i for i in range(14, 32)] +
                        ['v%d' % i for i in list(range(14, 32)) + list(range(64, 128))] + ['cr2', 'cr3', 'cr4'])
ABI_SKIPPED = [0, 0]  # (real calls with a reload left out, reloads left out)
PROMOTED_SITE = re.compile(r'^\tif \(REX_PROMOTED_IS\(')

REG = re.compile(r'\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|reserved)\b')
DEF = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
# A call of the generated code's convention (group 1: the name as written), an indirect call, a trap.
CALL = re.compile(r'\b([A-Za-z_]\w*)\(ctx, base\);|\bREX_CALL_INDIRECT_FUNC(?:_AT)?\([^;]*\);|\bppc_trap\(ctx, base[^;]*\);')
PROMOTED = re.compile(r'\bREX_PROMOTED_CALL\(0x[0-9A-F]{8}, (\w+)\)')
LEAF_REG = re.compile(r'\bREX_LEAF_LOCAL\(\w+, l_\w+, (\w+)\)')
RETURN = re.compile(r'\breturn;')
LABEL = re.compile(r'^loc_[0-9A-F]{8}:\s*$')
GOTO = re.compile(r'\bgoto loc_[0-9A-F]{8};')
# Every name the generator writes in front of a parenthesis (tools/loop_locals.py's list): anything else is a
# port's own host function (a mid-asm hook), which a copy cannot contain.
NAME = re.compile(r'([A-Za-z_][A-Za-z0-9_:]*(?:<[A-Za-z0-9_ ]+>)?)\s*\(')
KNOWN = re.compile(r'^(?:REX_\w+|simde_\w+|__builtin_\w+|rex::ppc::\w+|rex::ppc::exact::\w+|std::fma|std::bit_cast<\w+>|double|float|sqrt|'
                   r'memset|if|for|switch|sizeof|u?int(?:8|16|32|64)_t|u(?:8|16|32|64)|size_t|int|short|char|'
                   r'static_cast<[A-Za-z0-9_ ]+>|compare(?:<\w+>)?|setFromMask|(?:en|dis)ableFlushMode(?:Unconditional)?|'
                   r'flushModeIs(?:On|Off)|ppc_trap|simde__m128i?|'
                   # renut-ultimate (port 144): the spin-wait hints' yield (spin_hint_yield; no guest state)
                   r'rex_spin_hint)$')
# Forms that make a function unfit for a copy and for a closure (see the docstring).
SPECIAL = re.compile(r'\b(REX_GUEST_CAS|reserved|REX_MM_(?:STORE|LOAD)_\w+|ppc_setjmp|ppc_longjmp|REX_ENTER_GLOBAL_LOCK|'
                     r'REX_LEAVE_GLOBAL_LOCK|REX_CHECK_GLOBAL_LOCK|REX_QUERY_TIMEBASE|REX_UNIMPLEMENTED|'
                     r'std::atomic_thread_fence|REX_GUEST_SYNC|REX_GUEST_LWSYNC|REX_GUEST_EIEIO|storeFromGuest|'
                     r'loadFromHost|log2f)\b')
# Guest memory through a raw host address. Loads (the vector loads) and the two store forms the tool logs.
# banjo-nx (2026-10-23): the store-card form of a vector store too (package 34: REX_CARD(ea), then the store, one comma
# expression); the guard's Note goes in front of the whole expression, so the card and the store run together.
RAW_VECTOR_STORE = re.compile(r'^\t(?:REX_CARD\([^()]*\), )?simde_mm_store_si128\(\(simde__m128i\*\)REX_RAW_ADDR\(')
RAW_MEMSET = re.compile(r'^\tmemset\(\(void\*\)REX_RAW_ADDR\((.*)\), (\w+), (\d+)\);$')
RAW_LOAD = re.compile(r'^\tsimde_mm_store_si128\(\(simde__m128i\*\)\w+(?:\.\w+)+, .*REX_RAW_ADDR\(')
# sasrt-nx (2026-11-10, the user: "convert as much as you can the profiler picked up on to machine copies"): guest
# atomics in copies. The game's lwarx / stwcx. pairs (a reservation load into the function's local `reserved`, then a
# compare-and-swap on the same guest word), its sync / lwsync fences and log2f kept 3,741 of the race's 7,367 samples
# of generated code out of the copies (SASRT's handheld races lat10-lat14). A copy runs the very statements of the
# function, the registers in locals, so the running form (Fast) does the same atomic operations on the same guest
# words. The guard's forms (Logged and o_, the write log and the roll-back) cannot repeat an atomic operation (other
# threads may see it): each reservation load, compare-and-swap and fence is preceded by `if (s.Open()) return;`, so a
# logged run ends before the first one (the call is then not compared, as for a call outside the copy), and in the
# running form Open() is a constant false. log2f is a pure function. SASRT_LOCALIZE_ATOMICS=0: refused, as before.
ATOMICS = os.environ.get('SASRT_LOCALIZE_ATOMICS', '1') != '0'
# blur-nx (2026-11-16, tools/guest_fences.py): the guest fences as macros with the PowerPC orderings, the same rules.
ATOMIC_SPECIAL = {'reserved', 'REX_GUEST_CAS', 'std::atomic_thread_fence', 'REX_GUEST_SYNC', 'REX_GUEST_LWSYNC',
                  'REX_GUEST_EIEIO', 'log2f'}
ATOMIC_HOST = {'std::atomic_thread_fence', 'log2f'}
ATOMIC_LOAD = re.compile(r'^\t+reserved\.u(?:32|64) = \*\(uint(?:32|64)_t\*\)REX_RAW_ADDR\(ea\);$')
ATOMIC_CAS = re.compile(r'^\t+[\w.]+\.eq = REX_GUEST_CAS(?:_C)?\((?:ea, )?reinterpret_cast<uint(?:32|64)_t\*>\(REX_RAW_ADDR\(ea\)\), '
                        r'reserved\.s(?:32|64), __builtin_bswap(?:32|64)\([\w.]+\.s(?:32|64)\)\);$')
ATOMIC_FENCE = re.compile(r'^\t+(?:std::atomic_thread_fence\(std::memory_order_\w+\)|REX_GUEST_(?:SYNC|LWSYNC|EIEIO)\(\));$')


# sasrt-nx (2026-11-11, port 74; its idea 39): store-card runs. Every guest store outside the stack marks its page's card
# (tools/store_cards.py, after this tool). In the bulk copies (app/src/cpu/banjo_hot_prelude.h with SASRT_HOT_CARD_RUNS)
# a function keeps the page of its current run of stores in a local (REX_CARD_RUN_DECL) and marks a page when the run
# moves to another one, and the pending page at every point another thread can be told about the stores: before every
# call (a copy's callee included), fence and return, and at the function's end (REX_CARD_FLUSH); a compare-and-swap
# flushes and marks its own page (REX_GUEST_CAS_C in the prelude). So every store's page is still marked before the
# function's next synchronisation, which is what Renut's cached copies need (they are checked at a kick, which the game
# reaches through a call). Without SASRT_HOT_CARD_RUNS both macros are nothing (banjo_hot_common.h).
# SASRT_LOCALIZE_CARD_RUNS=0: not written.
CARD_RUNS = os.environ.get('SASRT_LOCALIZE_CARD_RUNS', '1') != '0'
# sasrt-nx (2026-11-11, port 74; its idea 52): in a loop, a register loaded from itself (`r7 = *(r7 + K)`: a linked
# list's next pointer) is followed by a prefetch of the node it now points at (REX_PREFETCH_GUEST, banjo_hot_common.h:
# a hint that cannot fault; CMake SASRT_HOT_PREFETCH=OFF compiles it out), so the node's line is on its way during the
# rest of the iteration. SASRT_LOCALIZE_PREFETCH=0: not written.
PREFETCH = os.environ.get('SASRT_LOCALIZE_PREFETCH', '1') != '0'
CHASE_SRC = re.compile(r'^\t((?:ctx\.)?r\d+|ll_r\d+)\.u64 = REX_LOAD_D_U32\(\1\.u32, -?\d+\);$')
CHASE = re.compile(r'^\t(l{1,2}_r\d+|r\d+)\.u64 = REX_LOAD_D_U32\(\1\.u32, -?\d+\);$')
# sasrt-nx (2026-11-11, port 74; its idea 60): tools/loop_barriers.py puts a compiler barrier at a loop header so a
# polling loop's guest load is not hoisted out of it (a thread spinning forever); the barrier also keeps every invariant
# guest load in the loop. A straight counted loop cannot spin on memory: its only way back is the count register's
# `bdnz`, nothing in it writes the count register, and it has no other branch, return or call. Its exit does not depend
# on a load, so a hoisted load only means another thread's write is seen after the (bounded) loop instead of during it.
# The copies' running form leaves those barriers out (the original statements' form keeps them); the compiler's own
# alias analysis then hoists what it can prove (same base, other offsets). ON unless SASRT_LOCALIZE_COUNTED_BARRIERS=1
# (=1: every barrier kept, idea 60 off; unset or any other value: the barriers of those loops left out).
COUNTED_NO_BARRIER = os.environ.get('SASRT_LOCALIZE_COUNTED_BARRIERS', '0') != '1'
LOOP_LABEL = re.compile(r'^(loc_[0-9A-F]+):\s*$')
CTR_DEC = re.compile(r'^\t--(?:ctx\.)?ctr\.u64;$')
CTR_BACK = re.compile(r'^\tif \((?:ctx\.)?ctr\.u32 != 0\) goto (loc_[0-9A-F]+);$')
CTR_WRITE = re.compile(r'\b(?:ctx\.)?ctr\.(?:u|s)(?:32|64) = ')
LOOP_STATS = [0, 0, 0]  # idea 52 prefetches, idea 60 barriers left out, M-19 rules applied
# blur-nx (2026-11-13, M-19 of docs/cpu-moonshots-50-1113.md and its audit): hand-named prefetches. Each line of
# app/src/cpu/hot/prefetch_rules.txt, `<function> <label> <statement>`, puts the statement right after that label in
# the copy's running form (and its logged twin, the same text; not in the original statements' form), written in the
# generated code's names (ctx.<register> becomes the copy's local, as everywhere). For the node loops whose callee first
# touches the next node's object cold: its first lines, prefetched one iteration ahead (REX_PREFETCH_GUEST cannot fault;
# a load the statement makes must be one the guest could make there, e.g. the next entry while one is left). A rule of a
# function that is copied but no longer has the label stops the tool (the guest code under the rule changed).
# SASRT_LOCALIZE_PREFETCH=0: none written.
PREFETCH_RULES_FILE = os.path.join(APP, 'src', 'cpu', 'hot', 'prefetch_rules.txt')


def load_prefetch_rules():
    rules = {}
    if not os.path.isfile(PREFETCH_RULES_FILE):
        return rules
    for number, raw in enumerate(open(PREFETCH_RULES_FILE, encoding='utf-8'), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(None, 2)
        if len(parts) != 3 or not re.fullmatch(r'(?:sub_)?[0-9A-Fa-f]{8}', parts[0]) or \
                not re.fullmatch(r'loc_[0-9A-F]{8}', parts[1]) or not parts[2].endswith(('}', ';')):
            sys.exit(f'localize_native: prefetch_rules.txt:{number}: not `<function> <label> <statement>`: {line}')
        name = parts[0] if parts[0].startswith('sub_') else 'sub_' + parts[0].upper()
        rules.setdefault(name, {}).setdefault(parts[1], []).append(parts[2])
    return rules


PREFETCH_RULES = load_prefetch_rules() if PREFETCH else {}
CONST_ARGS = os.environ.get('SASRT_LOCALIZE_CONST_ARGS', '1') != '0'  # port 74, its idea 34 (localize())
CONST_ARG_FORCED = [0]


def loop_info(lines):
    """(indices of loop-carried pointer loads, indices of the barrier lines of straight counted loops) of one function's lines."""
    labels = {}
    for i, l in enumerate(lines):
        m = LOOP_LABEL.match(l)
        if m:
            labels.setdefault(m.group(1), i)
    inside, drop, chase = set(), set(), set()
    backs = {}
    for j, l in enumerate(lines):
        for lab in re.findall(r'goto (loc_[0-9A-F]+);', l):
            if lab in labels and labels[lab] < j:
                backs.setdefault(lab, []).append(j)
    for lab, js in backs.items():
        i, end = labels[lab], max(js)
        inside.update(range(i, end + 1))
        # Idea 52: a pointer the loop carries (`p = p->next`): its only definitions in the loop are its own loads.
        for k in range(i, end + 1):
            mc = CHASE_SRC.match(lines[k])
            if not mc:
                continue
            reg = mc.group(1)
            defs = [q for q in range(i, end + 1) if re.match(r'^\t' + re.escape(reg) + r'\.[us](?:8|16|32|64) = ', lines[q])]
            if all(CHASE_SRC.match(lines[q]) and CHASE_SRC.match(lines[q]).group(1) == reg for q in defs):
                chase.add(k)
        if not COUNTED_NO_BARRIER:
            continue
        if i + 1 >= len(lines) or lines[i + 1].strip() != 'REX_LOOP_BARRIER();':
            continue
        bar = i + 1  # (sasrt-nx's batch 2 audit, finding 5: only the label's own barrier)
        ok = all(CTR_BACK.match(lines[j]) and j > 0 and CTR_DEC.match(lines[j - 1]) for j in js)
        for k in range(i + 1, end + 1):
            if not ok:
                break
            l = lines[k]
            if CTR_BACK.match(l) and CTR_BACK.match(l).group(1) == lab:
                continue
            if CTR_DEC.match(l):
                continue
            if 'goto ' in l or re.search(r'\breturn\b', l) or CALL.search(l) or 'switch (' in l or CTR_WRITE.search(l):
                ok = False
        if ok:
            drop.add(bar)
    return chase, drop


CARD_FLUSH = 'REX_CARD_FLUSH(); ' if CARD_RUNS else ''


def atomic_stop(l):
    """A reservation load, compare-and-swap or fence, with the guard's stop in front (see ATOMICS)."""
    if not ATOMICS or not (ATOMIC_LOAD.match(l) or ATOMIC_CAS.match(l) or ATOMIC_FENCE.match(l)):
        return l
    stripped = l.lstrip('\t')
    # Port 74 (its idea 39): a fence flushes the pending card run first; a compare-and-swap's REX_GUEST_CAS_C does it
    # itself (sasrt-nx's batch 2 audit, finding 1: no second flush).
    flush = CARD_FLUSH if ATOMIC_FENCE.match(l) else ''
    return l[:len(l) - len(stripped)] + '{ if (s.Open()) return; ' + flush + stripped + ' }'


ALL = None  # "reach unknown": every register


def order(reg):
    return (re.sub(r'\d', '', reg), int(re.sub(r'\D', '', reg) or 0))


def type_of(reg):
    if reg.startswith('cr'):
        return 'PPCCRRegister'
    if reg == 'xer':
        return 'PPCXERRegister'
    if reg.startswith('v'):
        return 'PPCVRegister'
    return 'PPCRegister'


# tools/inline_memcalls.py wraps a constant-size memcpy / memset call as REX_MEMCPY_CONST(n, sub_X(ctx, base)); (the
# helper runs the call when it must). A localized copy keeps the plain call (its callees are reached through their
# own localized or hand-written forms, which the wrapper would hide), so the wrapper is taken off where the text is read.
MEMCALL_WRAP = re.compile(r'\bREX_MEM(?:CPY|SET)_CONST\(\d+, ((?:__imp__)?sub_[0-9A-F]{8}\(ctx, base\))\);')


# banjo-nx (2026-10-23): tools/inline_leaves.py gives each generated file static copies of tiny leaves of other files
# (inl_sub_X, defined in that file only); a copy made from the text calls the leaf by its own name instead, which this
# tool reaches as any callee (the copies' shard files cannot see another file's statics).
INLINE_LEAF = re.compile(r'\b(?:REX_INL\((sub_[0-9A-F]{8})\)|inl_(sub_[0-9A-F]{8}))\(ctx, base\)')


# banjo-nx (2026-10-23): tools/r1_local.py (cpu_aggressive) keeps r1 in a local mirror, r1l, synced to ctx.r1 by
# REX_R1_SYNC / KEEP / RELOAD / ASSUMED. This tool's register model knows ctx.r1 only: copies made from the mirror form
# left r1 out of their hand-overs and of the guard's register set, and three bulk copies wrote a wrong back chain in the
# emulator's guarded run. The text is read as before that pass (the copies keep their own local of r1).
R1_LOCAL_DECL = re.compile(r'^\s*REX_R1_LOCAL\(\);\s*$')
R1_SYNC = re.compile(r'\s*REX_R1_(?:SYNC|KEEP|RELOAD|ASSUMED)\(\);')
R1_MIRROR = re.compile(r'\br1l\b')


def plain_r1(line):
    if 'REX_R1_' not in line and 'r1l' not in line:
        return line
    if R1_LOCAL_DECL.match(line):
        return ''
    return R1_MIRROR.sub('ctx.r1', R1_SYNC.sub('', line))


def plain_calls(line):
    line = plain_r1(line)
    if 'REX_MEM' in line:
        line = MEMCALL_WRAP.sub(r'\1;', line)
    if 'REX_INL(' in line or 'inl_sub_' in line:  # the macro form (REX_INL(sub_X)) as the generated text has it
        line = INLINE_LEAF.sub(lambda m: (m.group(1) or m.group(2)) + '(ctx, base)', line)
    return line


def unleaf(lines):
    """A function's lines as they were before tools/leaf_locals.py: ctx.<register> again, without the declarations and
    the stores before the returns (from sa2-nx's tool). This tool then makes its own locals of them."""
    return [text for _, text in unleaf_numbered(list(enumerate(lines)))]


def unleaf_numbered(pairs):
    """unleaf() on (line number, text) pairs: the lines that stay keep their numbers."""
    text = '\n'.join(l for _, l in pairs)
    regs = LEAF_REG.findall(text)
    if not regs:
        return pairs
    names = re.compile(r'\bl_(%s)\b' % '|'.join(re.escape(r) for r in regs))
    out = []
    for n, l in pairs:
        # REX_LEAF_DEAD_R / _F (tools/leaf_dead_stores.py) are write-backs no caller reads: in the ctx form every
        # register is written where the function writes it, so they go like the stores.
        if re.match(r'\tREX_LEAF_LOCAL\(', l) or re.match(r'\t(?:REX_LEAF_(?:STORE|DEAD_R|DEAD_F)\(l_\w+, \w+\);\s?)+$', l):
            continue
        l = re.sub(r'\{ (?:REX_LEAF_(?:STORE|DEAD_R|DEAD_F)\(l_\w+, \w+\); )+return; \}', 'return;', l)
        out.append((n, names.sub(lambda m: 'ctx.' + m.group(1), l)))
    if 'REX_LEAF_' in '\n'.join(l for _, l in out):
        sys.exit('localize_native: a leaf-local form this tool does not know (tools/leaf_locals.py changed?)')
    return out


def written_registers(regs, text):
    """The registers a function can have changed (tools/leaf_locals.py's rule for r and f; vectors by their forms)."""
    written = []
    for r in regs:
        if r[0] in 'rf' and r[1:].isdigit():
            mentions = re.findall(r'\bctx\.' + r + r'\b(\.\w+)?(\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?', text)
            if all(field and not assign for field, assign in mentions) and not re.search(r'&\s*ctx\.' + r + r'\b', text):
                continue
        elif r[0] == 'v' and r[1:].isdigit():
            only_read = True
            for m in re.finditer(r'\bctx\.' + r + r'\b', text):
                before = text[max(0, m.start() - 48):m.start()]
                after = text[m.end():m.end() + 80]
                if re.search(r'simde_mm_load_(?:si128\(\(simde__m128i\*\)|ps\()$', before):
                    continue  # a vector load of the register
                a = re.match(r'\.\w+\[[^\]]*\](\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?', after)
                if a and not a.group(1):
                    continue  # one element read
                only_read = False
                break
            if only_read:
                continue
        written.append(r)
    return written


def summarise(name, lines):
    """What the index keeps of a function (its code lines without comments, in ctx form)."""
    text = '\n'.join(lines)
    regs = sorted({m.group(1) for m in REG.finditer(text)}, key=order)
    callees = []
    unknown = 0
    traps = 0
    promoted = 0  # blur-nx (2026-11-13, M-25): of `unknown`, the fallbacks of promoted sites
    for l in lines:
        for m in CALL.finditer(l):
            if m.group(1) is None:
                if m.group(0).startswith('ppc_trap('):
                    traps += 1
                else:
                    unknown += 1
                    if PROMOTED_SITE.match(l):
                        promoted += 1
            elif m.group(1) not in callees:
                callees.append(m.group(1))
    if traps:
        # The trap helper (the generated pch) reads r3 and r4 of the context for its message and writes nothing.
        regs = sorted(set(regs) | {'r3', 'r4'}, key=order)
    special = sorted({m.group(1) for m in SPECIAL.finditer(text)} - (ATOMIC_SPECIAL if ATOMICS else set()))
    for l in lines:
        for m in NAME.finditer(l):
            n = m.group(1)
            if KNOWN.match(n) or l[m.end():].startswith('ctx, base)'):
                continue
            if ATOMICS and n in ATOMIC_HOST:  # sasrt-nx (2026-11-10): see ATOMICS
                continue
            special.append('host function ' + n)
        if 'REX_RAW_ADDR(' in l and not (RAW_VECTOR_STORE.match(l) or RAW_MEMSET.match(l) or RAW_LOAD.match(l) or
                                         (ATOMICS and (ATOMIC_LOAD.match(l) or ATOMIC_CAS.match(l)))):
            special.append('raw guest access')
    return {'regs': regs, 'written': written_registers(regs, text), 'callees': callees, 'unknown': unknown,
            'promoted': promoted, 'traps': traps, 'special': sorted(set(special)), 'lines': len(lines)}


class Index:
    """Every generated function: where its body is and its summary (cached by the files' modification times)."""

    def __init__(self, gen):
        self.gen = gen
        self.files = {}
        paths = sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp')))
        if not paths:
            sys.exit('localize_native: no *_recomp.N.cpp in ' + gen)
        stamp = [[os.path.basename(p), os.path.getmtime(p), os.path.getsize(p)] for p in paths]
        # sasrt-nx (2026-11-11, port 74; port 70's follow-up, sasrt-nx's 2026-11-10 audit finding 2): ATOMICS changes
        # summarise(), so the cache key names it (a warm cache from the other setting would copy atomic functions
        # without their stops). Every cache written before this key is rebuilt once.
        me = f'{INDEX_FORMAT}-atomics{int(ATOMICS)}'
        try:
            cached = json.load(open(INDEX, encoding='utf-8'))
            if cached.get('stamp') == stamp and cached.get('gen') == os.path.abspath(gen) and cached.get('tool') == me:
                self.funcs = cached['funcs']
                return
        except (OSError, ValueError):
            pass
        self.funcs = {}
        for p in paths:
            lines = open(p, encoding='utf-8', errors='replace').read().split('\n')
            i = 0
            while i < len(lines):
                m = DEF.match(lines[i])
                if not m:
                    i += 1
                    continue
                j = i + 1
                while j < len(lines) and lines[j] != '}':
                    j += 1
                code = unleaf([plain_calls(l) for l in lines[i + 1:j] if not l.lstrip().startswith('//')])
                s = summarise(m.group(1), code)
                s['file'] = os.path.basename(p)
                s['at'] = [i + 1, j]
                self.funcs[m.group(1)] = s
                i = j + 1
        os.makedirs(os.path.dirname(INDEX), exist_ok=True)
        json.dump({'gen': os.path.abspath(gen), 'stamp': stamp, 'tool': me, 'funcs': self.funcs},
                  open(INDEX, 'w', encoding='utf-8'))

    def body(self, name):
        f = self.funcs[name]
        if f['file'] not in self.files:
            self.files[f['file']] = open(os.path.join(self.gen, f['file']), encoding='utf-8',
                                         errors='replace').read().split('\n')
        a, b = f['at']
        lines = self.files[f['file']]
        # (1-based line number in the generated file, text), comments left out.
        return unleaf_numbered([(k + 1, plain_calls(lines[k])) for k in range(a, b)
                                if not lines[k].lstrip().startswith('//')])


def strip_comments(text, toml=False):
    if toml:
        return re.sub(r'#[^\n]*', ' ', text)
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def list_entries(path=LIST):
    """The functions of app/src/cpu/hot/localized.txt: [name, options, what]."""
    out = []
    if not os.path.isfile(path):
        return out
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        name = parts[0] if not re.fullmatch(r'[0-9A-Fa-f]{8}', parts[0]) else 'sub_' + parts[0].upper()
        opts = {}
        k = 1
        while k < len(parts) and re.fullmatch(r'[a-z_]+(=\S+)?', parts[k]) and parts[k].split('=')[0] in (
                'off', 'key', 'closure', 'member', 'inline'):
            key, _, value = parts[k].partition('=')
            opts[key] = value or True
            k += 1
        out.append([name, opts, ' '.join(parts[k:])])
    return out


def hand_entries(path=HAND):
    """The hand-written natives of app/src/cpu/hot/hand.txt: {generated name: {'id': namespace suffix, 'exact': set}}."""
    out = {}
    if not os.path.isfile(path):
        return out
    for line in open(path, encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        parts = line.split()
        name = parts[0] if not re.fullmatch(r'[0-9A-Fa-f]{8}', parts[0]) else 'sub_' + parts[0].upper()
        entry = {'id': identifier(name), 'exact': set(), 'calls': []}
        for part in parts[1:]:
            key, _, value = part.partition('=')
            if key == 'exact':
                entry['exact'] = set(value.split(','))
            elif key == 'id':
                entry['id'] = value
            elif key == 'inexact':
                # blur-nx (2026-11-16): an inexact native (audio) declines while the guard runs, so the guard compares
                # the exact copy: it adds nothing to the hook's skip set.
                entry['inexact'] = True
            elif key == 'calls':
                # blur-nx (2026-11-16, the 15 ms batch): a native that calls guest functions (see calls_struct).
                entry['calls'] = ['sub_' + c.upper() if re.fullmatch(r'[0-9A-Fa-f]{8}', c) else c
                                  for c in value.split(',') if c]
            else:
                sys.exit(f'localize_native: hand.txt: {part!r} in the line of {name}')
        out[name] = entry
    return out


def native_call(entry):
    """The call of a hand-written native: `::banjo::hand::n_<id>::Native` with, for one that calls guest functions
    (hand.txt calls=), the header's struct of those calls as its first template argument."""
    if entry.get('calls'):
        return '::banjo::hand::n_%s::Native<Calls_%s>' % (entry['id'], entry['id'])
    return '::banjo::hand::n_%s::Native' % entry['id']


def calls_struct(entry, member_set, externs, hand):
    """blur-nx (2026-11-16, the 15 ms batch): the guest calls of a hand-written native (hand.txt calls=), for one copy
    header. A native that calls guest functions gets this struct as its template argument C and calls C::sub_X(ctx,
    base, s) with the callee's arguments in ctx, as the guest code would; then `if (s.Stopped()) return true;`.
      - A callee in this header's closure runs as its copy f_sub_X (any form: the guard's logged run follows it, its
        stores recorded).
      - Any other callee is a real call of the generated function (the in-place copy on the Switch); in the guard's
        logged forms Open() stops the run there (the call is then not compared), as for the copies' own real calls.
    The native hands over registers through ctx itself (it holds none of the guest's), and sets ctx.r1 to the frame its
    original allocates, so the callees' frames land where they would."""
    lines = ['struct Calls_%s {' % entry['id']]
    for c in entry['calls']:
        lines.append('  template <class S>')
        if c in member_set and c in hand:
            # A callee with a native of its own (the node loop's child mixers: itself): the native first, as at the
            # copies' own call sites.
            lines.append('  [[gnu::always_inline]] static inline void %s(PPCContext& __restrict ctx, uint8_t* base, '
                         'S& s) { if (!%s(ctx, base, s)) f_%s(ctx, base, s); }' % (c, native_call(hand[c]), c))
        elif c in member_set:
            lines.append('  [[gnu::always_inline]] static inline void %s(PPCContext& __restrict ctx, uint8_t* base, '
                         'S& s) { f_%s(ctx, base, s); }' % (c, c))
        else:
            externs.add(c)
            # The generated function by its global name: inside the struct the bare name is the member itself.
            lines.append('  [[gnu::always_inline]] static inline void %s(PPCContext& __restrict ctx, uint8_t* base, '
                         'S& s) { if (s.Open()) return; ::%s(ctx, base); }' % (c, c))
    lines.append('};')
    return lines


def skip_literal(regs):
    """The guard's RegSkip of banjo_hot_common.h for a set of register names."""
    gpr = fpr = cr = 0
    vr = [0, 0]
    ctr = xer = False
    for r in regs:
        if r == 'ctr':
            ctr = True
        elif r == 'xer':
            xer = True
        elif r.startswith('cr'):
            cr |= 1 << int(r[2:])
        elif r.startswith('r') and r[1:].isdigit():
            gpr |= 1 << int(r[1:])
        elif r.startswith('f'):
            fpr |= 1 << int(r[1:])
        elif r.startswith('v'):
            n = int(r[1:])
            vr[n >> 6] |= 1 << (n & 63)
        else:
            sys.exit(f'localize_native: no skip bit for register {r}')
    return ('{0x%08Xu, 0x%08Xu, {0x%016Xull, 0x%016Xull}, 0x%02Xu, %s, %s}'
            % (gpr, fpr, vr[0], vr[1], cr, 'true' if ctr else 'false', 'true' if xer else 'false'))


def strip_windows_only(text):
    """blur-nx (2026-11-13, M-28 of docs/cpu-moonshots-50-1113.md): the text without its Windows-only branches (#if
    defined(_WIN32) / #ifdef _WIN32 up to the matching #else, #elif or #endif; an #else branch is kept, the Switch
    compiles it). Code the Switch never compiles hooks nothing there. In Blur only blur_write_watch.cpp/.h has such
    branches naming guest addresses: a PC diagnostic that wraps sub_8216AE98, sub_8216CA28 and sub_8256D6D8 through the
    indirect-call table (SetFunction) and calls their __imp__ originals, which a copy leaves as they are; it kept those
    three out of the copies (0.1-0.2 ms a frame)."""
    out, skip_depth, depth = [], None, 0
    for line in text.split('\n'):
        d = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', line)
        if d:
            kind, rest = d.group(1), d.group(2)
            if kind in ('if', 'ifdef', 'ifndef'):
                depth += 1
                windows = (kind == 'ifdef' and rest.strip() == '_WIN32') or \
                          (kind == 'if' and re.fullmatch(r'\s*defined\s*\(?\s*_WIN32\s*\)?\s*(//.*)?', rest))
                if skip_depth is None and windows:
                    skip_depth = depth
                    continue
            elif kind in ('else', 'elif') and skip_depth == depth:
                skip_depth = None  # the other branch is compiled on the Switch
                continue
            elif kind == 'endif':
                depth -= 1
                if skip_depth is not None and depth < skip_depth:
                    skip_depth = None
                    continue
        if skip_depth is None:
            out.append(line)
    return '\n'.join(out)


class Hooks:
    """What an app or SDK hook may replace: every 82xxxxxx (83xxxxxx: port 144) address and every identifier their sources' code mentions
    (comments and __imp__ references left out, as in sa2-nx's tool: a comment hooks nothing and __imp__sub_X is the
    original). The functions of localized.txt are not in it: a copy touches no more than its original."""

    def __init__(self, natives):
        self.addresses = set()
        self.words = set()
        # renut-ultimate (2026-11-16, port 144; tw2de_nx's): 83xxxxxx too when the game's code runs there (its function
        # table has entries at 0x83000000 and above: TW2's sub_833C2518 ...); a tree without them reads as before.
        addr_re = r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])'
        for p in glob.glob(os.path.join(GEN, '*_init.cpp')):
            if re.search(r'\{ 0x83[0-9A-Fa-f]{6},', open(p, encoding='utf-8', errors='ignore').read()):
                addr_re = r'(?<![0-9A-Fa-f])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])'
        roots = [os.path.join(APP, 'src'), os.path.join(ROOT, 'sdk', 'src'), os.path.join(ROOT, 'sdk', 'include')]
        # sasrt-nx (2026-11-11, port 74; its idea 29): not app/config/renut_gpu_funcs.toml any more. It is a game's map of
        # Direct3D's names (a reference: no code hooks through it; this base's Renut hooks Direct3D by address,
        # BANJO_RENUT_HOOK(sub_X), which the sources below name), and it kept every function it names out of the copies
        # (in sasrt-nx 0.4 % of a race's samples, plain). A function a source really hooks is still found in the source.
        # Only this list changed: tools/direct_calls.py (SOURCES) and tools/post_codegen.py (HOOK_TOMLS, for
        # flush_mode_flow.py) still read the file, as before port 74. (Neither Katamari nor rr6-nx has one.)
        tomls = [os.path.join(APP, 'config', 'renut_hooks.toml'),
                 os.path.join(APP, 'renut_config.toml'), os.path.join(APP, 'renut_manifest.toml')]
        paths = [(p, True) for p in tomls if os.path.isfile(p)]
        for root in roots:
            for d, _, fs in os.walk(root):
                if os.path.abspath(d).startswith(os.path.abspath(os.path.join(APP, 'src', 'cpu', 'hot'))):
                    continue  # the list of the natives themselves
                for fn in fs:
                    if fn.endswith(('.cpp', '.h', '.inc', '.hpp', '.c')):
                        paths.append((os.path.join(d, fn), False))
        for path, toml in paths:
            try:
                text = open(path, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            text = strip_comments(text, toml)
            if not toml:
                text = strip_windows_only(text)  # blur-nx (2026-11-13, M-28)
            text = re.sub(r'__imp__\w+', ' ', text)
            self.addresses.update(m.upper() for m in re.findall(addr_re, text))
            if not toml:
                self.words.update(re.findall(r'\b[A-Za-z_]\w{5,}\b', text))
        self.natives = set(natives)

    def replaced(self, name):
        if name in self.natives:
            return False
        if re.fullmatch(r'sub_[0-9A-F]{8}', name):
            return name[4:] in self.addresses
        return name in self.words


class Reach:
    """For a call: the registers the callee and everything under it mention, and those they can change (strongly
    connected components over the call graph, on demand; sa2-nx's). ALL = unknown."""

    def __init__(self, index, hooks):
        self.funcs = index.funcs
        self.hooks = hooks
        self.memo = {}

    def own(self, name):
        imp = name.startswith('__imp__')
        base = name[7:] if imp else name
        f = self.funcs.get(base)
        if f is None or f['unknown'] or (not imp and self.hooks.replaced(base)):
            return ALL, ALL, ()
        if any(s.startswith('host function') or 'setjmp' in s or 'longjmp' in s for s in f['special']):
            return ALL, ALL, ()
        return set(f['regs']), set(f['written']), list(f['callees'])

    def of(self, root):
        if root in self.memo:
            return self.memo[root]
        index, low, on_stack, stack, owns = {}, {}, set(), [], {}
        counter = 0
        frames = []

        def push(n):
            nonlocal counter
            index[n] = low[n] = counter
            counter += 1
            stack.append(n)
            on_stack.add(n)
            owns[n] = self.own(n)
            frames.append([n, owns[n][2], 0])

        push(root)
        while frames:
            frame = frames[-1]
            n, callees, pos = frame
            if pos < len(callees):
                frame[2] += 1
                c = callees[pos]
                if c in self.memo:
                    continue
                if c not in index:
                    push(c)
                elif c in on_stack:
                    low[n] = min(low[n], index[c])
                continue
            frames.pop()
            if frames:
                parent = frames[-1][0]
                low[parent] = min(low[parent], low[n])
            if low[n] != index[n]:
                continue
            members = []
            while True:
                w = stack.pop()
                on_stack.discard(w)
                members.append(w)
                if w == n:
                    break
            member_set = set(members)
            mention, change = set(), set()
            for w in members:
                regs, wr, callees = owns[w]
                if regs is ALL:
                    mention = ALL
                    break
                mention |= regs
                change |= wr
                for c in callees:
                    if c in member_set:
                        continue
                    rc = self.memo.get(c, (ALL, ALL))
                    if rc[0] is ALL:
                        mention = ALL
                        break
                    mention |= rc[0]
                    change |= rc[1]
                if mention is ALL:
                    break
            for w in members:
                self.memo[w] = (ALL, ALL) if mention is ALL else (mention, change)
        return self.memo[root]


ONLY_GOTO = re.compile(r'^\tgoto (loc_[0-9A-F]{8});$')
ANY_GOTO = re.compile(r'\bgoto (loc_[0-9A-F]{8});')
ONLY_RETURN = re.compile(r'^\t+(?:return;|\{ return; \})$')
# A statement that is nothing but a call (or the two alternative calls of a promoted indirect call).
ONLY_CALL = re.compile(r'^\t+(?:[A-Za-z_]\w*\(ctx, base\);|REX_CALL_INDIRECT_FUNC(?:_AT)?\([^;]*\);|'
                       r'ppc_trap\(ctx, base[^;]*\);)$|^\tif \(REX_PROMOTED_IS\(')
SWITCH_LINE = re.compile(r'^\t(?:case \d+:|default:)$|^\t\t(?:goto loc_[0-9A-F]{8};|__builtin_trap\(\);.*)$')


def line_writes(line):
    """The context registers one statement (in ctx form) can change. Everything that is not plainly a read counts."""
    if 'REX_LOOP_IN(' in line or 'REX_LOOP_LOCAL(' in line:
        return set()  # tools/loop_locals.py: a loop local takes the register's value
    out = set()
    for m in REG.finditer(line):
        r = m.group(1)
        before = line[max(0, m.start() - 48):m.start()]
        after = line[m.end():m.end() + 80]
        if r[0] in 'rf' and r[1:].isdigit():
            a = re.match(r'(\.\w+)?(\[[^\]]*\])?(\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?', after)
            if a.group(1) and not a.group(3) and not re.search(r'&\s*$', before):
                continue  # a field read
        elif r[0] == 'v' and r[1:].isdigit():
            if re.search(r'simde_mm_load_(?:si128\(\(simde__m128i\*\)|ps\()$', before):
                continue  # a vector load of the register
            a = re.match(r'\.\w+\[[^\]]*\](\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?', after)
            if a and not a.group(1):
                continue  # one element read
        out.add(r)
    return out


def dirty_sets(name, lines, call_mentions):
    """For every statement: the registers whose local can differ from the context when the statement starts (changed
    on some path since the function's entry or since the last call that stored them). A return stores only those, a
    call only those its callee can reach: the path that changes two registers does not pay for the forty another
    path changes. call_mentions(line) -> the reach of each call on the line (ALL = everything).
    Returns ([set per line, None where the line is never reached], writes per line, the set at the function's end)."""
    n = len(lines)
    label_at = {}
    for i, l in enumerate(lines):
        if LABEL.match(l):
            label_at[l.strip()[:-1]] = i
    succ = [None] * n
    writes = [set() for _ in range(n)]
    i = 0
    while i < n:
        l = lines[i]
        if l.startswith('\tswitch ('):
            j = i + 1
            targets = set()
            while j < n and lines[j] != '\t}':
                m = ANY_GOTO.search(lines[j])
                if not SWITCH_LINE.match(lines[j]):
                    sys.exit(f'localize_native: {name}: a switch body line this tool does not know: {lines[j].strip()}')
                if m:
                    targets.add(label_at[m.group(1)])
                j += 1
            succ[i] = sorted(targets)  # a jump table: every case jumps, the default traps
            writes[i] = line_writes(l)
            i = j + 1
            continue
        m = ONLY_GOTO.match(l)
        if m:
            succ[i] = [label_at[m.group(1)]]
        elif ONLY_RETURN.match(l):
            succ[i] = []
        else:
            succ[i] = [i + 1] + [label_at[g.group(1)] for g in ANY_GOTO.finditer(l)]
        writes[i] = line_writes(l)
        i += 1
    dirty = [None] * (n + 1)  # n: falling off the end
    dirty[0] = frozenset()
    work = [0]
    while work:
        i = work.pop()
        if i >= n or succ[i] is None:
            continue
        d = set(dirty[i]) | writes[i]
        mentions = call_mentions(lines[i])
        if mentions and ONLY_CALL.match(lines[i]):
            # The call stores what its callee can reach (and loads back what the callee can change, a part of it):
            # those locals equal the context again. Two alternative calls: only what both store.
            stored = None
            for mention in mentions:
                s = set(d) if mention is ALL else (d & mention)
                stored = s if stored is None else (stored & s)
            d -= stored
        for j in succ[i]:
            merged = frozenset(d) if dirty[j] is None else (dirty[j] | d)
            if merged != dirty[j]:
                dirty[j] = merged
                work.append(j)
    return dirty[:n], writes, dirty[n]


def base_name(written):
    return written[7:] if written.startswith('__imp__') else written


def fits(index, hooks, name):
    """Why a function cannot be in a closure (or be a copy's root), or None."""
    f = index.funcs.get(name)
    if f is None:
        return 'not a generated function'
    if f['special']:
        return ', '.join(f['special'])
    return None


def closed_tree(index, hooks, name, limit, memo):
    """The functions under `name` (itself included) when every one of them can be a copy and none makes a call that
    cannot be repeated (indirect, trap aside, import, hooked); else a string saying why not. `name` as a base name."""
    if name in memo:
        return memo[name]
    seen = []
    stack = [name]
    why = None
    lines = 0
    while stack and why is None:
        n = stack.pop()
        if n in seen:
            continue
        f = index.funcs.get(n)
        why = fits(index, hooks, n)
        if why is None and f['unknown'] - (f.get('promoted', 0) if PROMOTED_MEMBERS else 0):  # M-25
            why = 'it (or %s under it) makes an indirect call' % n if n != name else 'it makes an indirect call'
        if why is not None:
            if n != name and not why.startswith('it (or'):
                why = '%s under it: %s' % (n, why)
            break
        seen.append(n)
        lines += f['lines']
        if lines > limit:
            why = 'its call tree is over %d lines' % limit
            break
        for written in f['callees']:
            b = base_name(written)
            if b not in index.funcs:
                why = 'it (or %s under it) calls %s' % (n, written)
                break
            if not written.startswith('__imp__') and hooks.replaced(b):
                why = 'an app hook may replace %s' % b
                break
            stack.append(b)
    memo[name] = why if why is not None else seen
    return memo[name]


def closure_of(index, hooks, root, max_lines, max_member):
    """The root and the direct callees that come along as copies: those whose whole call tree can be repeated by the
    guard and is small (a copy is for the hot path; the cold subsystems a function reaches stay real calls). A call
    of the root to itself is a call to its own copy. Returns (members in order, {name as written: why it stays a
    real call})."""
    members = [root]
    left_out = {}
    total = index.funcs[root]['lines']
    memo = {}
    for written in index.funcs[root]['callees']:
        b = base_name(written)
        if b in members:
            continue
        why = None
        if not written.startswith('__imp__') and hooks.replaced(b):
            why = 'an app hook may replace it'
        tree = None
        if why is None:
            tree = closed_tree(index, hooks, b, max_member, memo)
            if isinstance(tree, str):
                why = tree
            elif root in tree:
                why = 'it calls back'
        if why is None:
            extra = [m for m in tree if m not in members]
            size = sum(index.funcs[m]['lines'] for m in extra)
            if total + size > max_lines:
                why = 'the closure is full'
            else:
                members.extend(extra)
                total += size
        if why is not None:
            left_out[written] = why
    return members, left_out


def shown(path):
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:  # another drive
        return path


def fnv1a64(data, h=0xCBF29CE484222325):
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


_ADDRESSES = None


def addresses(gen):
    """name -> (guest address, bytes up to the next function) from the generated function table."""
    global _ADDRESSES
    if _ADDRESSES is None:
        text = open(gen_file(gen, 'init.cpp'), encoding='utf-8').read()
        table = [(int(a, 16), n) for a, n in re.findall(r'^\t\{ 0x([0-9A-F]{8}), (\w+) \},$', text, re.M)]
        table.sort()
        _ADDRESSES = {}
        for k, (a, n) in enumerate(table):
            end = table[k + 1][0] if k + 1 < len(table) else a + 4
            _ADDRESSES[n] = (a, end - a)
    return _ADDRESSES


def identifier(name):
    return name[4:] if re.fullmatch(r'sub_[0-9A-F]{8}', name) else name


# tools/vec_unaligned_stores.py writes stvlx / stvrx as REX_STVLX / REX_STVRX (one helper call each, not a REX_STORE_
# form). banjo-nx (2026-10-22): the logged forms record the bytes they cover (stvlx: ea up to the next 16-byte
# boundary; stvrx: from the boundary below ea up to ea, nothing when ea is aligned). Before, the guard neither rolled
# them back nor compared them: a guarded call made those stores four times, and sub_82226330's guard (bulk copies,
# emulator) reported a difference that repeated (its first callee writes a matrix with them). The Fast form keeps the
# helper: its Note is a constant true.
# banjo-nx (2026-10-23): with the store-card form too (REX_STVLX_C / REX_STVRX_C): without it the logged forms missed
# them again and sub_82226330's guard reported its old difference (batch 5's emulator run).
VEC_LR = re.compile(r'^(\t+)REX_STV([LR])X(_C)?\(ea, ([\w.\[\]]+)\);$')


def store_line(l):
    """A line with its guest stores recorded first (the Fast policy's Note is a constant true)."""
    stores = False
    if 'REX_STORE_' in l:
        l = l.replace('REX_STORE_', 'BANJO_HOT_STORE_')
        stores = True
    m = VEC_LR.match(l)
    if m:
        span = ('(u32)(ea), 16u - ((u32)(ea) & 0xFu)' if m.group(2) == 'L'
                else '(u32)(ea) & ~0xFu, (u32)(ea) & 0xFu')
        l = '%sif (s.Note(%s)) REX_STV%sX%s(ea, %s);' % (m.group(1), span, m.group(2), m.group(3) or '', m.group(4))
        stores = True
    elif any(f in l for f in ('REX_STVLX(', 'REX_STVRX(', 'REX_STVLX_C(', 'REX_STVRX_C(')):
        sys.exit('localize_native: an unaligned vector store in a form this tool does not record: ' + l.strip()[:120])
    if RAW_VECTOR_STORE.match(l):
        at = l.index('REX_RAW_ADDR(') + len('REX_RAW_ADDR(')
        depth, k = 1, at
        while depth:
            depth += {'(': 1, ')': -1}.get(l[k], 0)
            k += 1
        l = '\tif (s.Note((u32)(%s), 16)) %s' % (l[at:k - 1], l.lstrip('\t'))
        stores = True
    m = RAW_MEMSET.match(l)
    if m:
        l = '\tif (s.Note((u32)(%s), %s)) %s' % (m.group(1), m.group(3), l.lstrip('\t'))
        stores = True
    return l, stores


# sasrt-nx (2026-11-11, port 74; its idea 57, sasrt-nx's merge of 2026-11-08 with its round A fix): two store idioms of
# the game's VMX code, merged in the copies' running form (f_*; the original statements' logged form o_* keeps them, so
# the guard compares against the original stores). Both write exactly the bytes the original writes and mark the same
# store cards. Switch back: SASRT_HOT_MERGED_STORES=OFF (CMake), which compiles the original statements
# (SASRT_HOT_MERGED_STORES_OFF).
#   - stvlx at ea, stvrx at ea + 16: the 16 bytes of one vector at [ea, ea + 16) (the generated helpers store them in
#     8/4/2/1-byte pieces chosen by the address): one 16-byte store when the second address is the first + 16 at run
#     time and the range does not cross the PC's physical-view boundary;
#   - four stvewx of one vector at ea, ea + a, ea + b, ea + c (each the lane its address selects; the generated code
#     spills the vector and loads each lane back by a computed index): one 16-byte store of the four words when a, b,
#     c are 4, 8, 12 at run time (::banjo::hand::detail::StoreWordsOfVector).
# The address expressions are register arithmetic only (no loads), so evaluating the second one before the first store
# reads what the original reads. The patterns take the plain forms (and the store-card forms, _C): post_codegen.py takes
# the cards out (store_cards.py --restore) before the copies are made and puts them back after (sasrt-nx's round A
# audit: matching only the _C forms, the merge never fired). The merged store marks its own cards (REX_CARD, a no-op
# without store cards), and store_cards.py gives the original statements of the fallback branch theirs.
MS_ADDR = r'[\w.]+(?: [+-] [\w.]+)?'
MS_PAIR = re.compile(r'(?P<i>\t+)ea = (?P<e1>' + MS_ADDR + r');\n'
                     r'(?P=i)if \(s\.Note\(\(u32\)\(ea\), 16u - \(\(u32\)\(ea\) & 0xFu\)\)\) REX_STVLX(?:_C)?\(ea, (?P<v>\w+)\);\n'
                     r'(?P=i)if \(s\.Stopped\(\)\) return;\n'
                     r'(?P=i)ea = (?P<e2>' + MS_ADDR + r');\n'
                     r'(?P=i)if \(s\.Note\(\(u32\)\(ea\) & ~0xFu, \(u32\)\(ea\) & 0xFu\)\) REX_STVRX(?:_C)?\(ea, (?P=v)\);\n'
                     r'(?P=i)if \(s\.Stopped\(\)\) return;\n')
MS_STORE = r'(?P=i)BANJO_HOT_STORE_U32(?:_C)?\(ea, (?P=v)\.u32\[3 - \(\(ea & 0xF\) >> 2\)\]\);\n(?P=i)if \(s\.Stopped\(\)\) return;\n'
MS_QUAD = re.compile(r'(?P<i>\t+)ea = \((?P<x>\w+)\.u32\) & ~0x3;\n'
                     r'(?P=i)BANJO_HOT_STORE_U32(?:_C)?\(ea, (?P<v>\w+)\.u32\[3 - \(\(ea & 0xF\) >> 2\)\]\);\n(?P=i)if \(s\.Stopped\(\)\) return;\n'
                     r'(?P=i)ea = \((?P=x)\.u32 \+ (?P<a>\w+)\.u32\) & ~0x3;\n' + MS_STORE +
                     r'(?P=i)ea = \((?P=x)\.u32 \+ (?P<b>\w+)\.u32\) & ~0x3;\n' + MS_STORE +
                     r'(?P=i)ea = \((?P=x)\.u32 \+ (?P<c>\w+)\.u32\) & ~0x3;\n' + MS_STORE)
F_FUNC = re.compile(r'^[^\n]*\bvoid f_\w+\(PPCContext& __restrict ctx, uint8_t\* base, \[\[maybe_unused\]\] S& s\) \{\n.*?^\}\n',
                    re.M | re.S)


MERGED = [0, 0, 0]  # stvlx / stvrx pairs, stvewx quads, headers with either
# blur-nx (2026-11-11): the stvewx lane as tools/vec_lane_index.py writes it, and the indexed form MS_QUAD matches.
LANE_FORM = re.compile(r'REX_LANE_U32\((\w+), 3 - \(\(ea & 0xF\) >> 2\)\)')
IDX_FORM = re.compile(r'(?<![\w.])(\w+)\.u32\[3 - \(\(ea & 0xF\) >> 2\)\]')


def merge_vector_stores(text):
    """The header's text with the two idioms merged inside the f_* functions; (text, pairs, quads)."""
    counts = [0, 0]

    def pair(m):
        counts[0] += 1
        i, e1, e2, v = m.group('i', 'e1', 'e2', 'v')
        orig = m.group(0)
        return (f'#if !defined(SASRT_HOT_MERGED_STORES_OFF)\n'
                f'{i}if (uint32_t({e2}) == uint32_t({e1}) + 16u && ::banjo::hand::detail::Straight(uint32_t({e1}), 16u)) [[likely]] {{\n'
                f'{i}\tea = {e1};\n'
                f'{i}\tif (s.Note((u32)(ea), 16u)) {{ simde_mm_storeu_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*){v}.u8))); REX_CARD(ea); REX_CARD(ea + 16u); }}\n'
                f'{i}\tif (s.Stopped()) return;\n'
                f'{i}\tea = {e2};\n'
                f'{i}}} else {{\n#endif\n'
                + orig +
                f'#if !defined(SASRT_HOT_MERGED_STORES_OFF)\n{i}}}\n#endif\n')

    def quad(m):
        counts[1] += 1
        i, x, v, a, b, c = m.group('i', 'x', 'v', 'a', 'b', 'c')
        orig = m.group(0)
        return (f'#if !defined(SASRT_HOT_MERGED_STORES_OFF)\n'
                f'{i}if ({a}.u32 == 4u && {b}.u32 == 8u && {c}.u32 == 12u && ::banjo::hand::detail::Straight({x}.u32 & ~0x3u, 16u)) [[likely]] {{\n'
                f'{i}\tif (!::banjo::hand::detail::StoreWordsOfVector(base, s, {x}.u32 & ~0x3u, {v})) return;\n'
                f'{i}\tea = ({x}.u32 + {c}.u32) & ~0x3;\n'
                f'{i}}} else {{\n#endif\n'
                + orig +
                f'#if !defined(SASRT_HOT_MERGED_STORES_OFF)\n{i}}}\n#endif\n')

    def func(m):
        # blur-nx (2026-11-11): tools/vec_lane_index.py (before the copies) writes the stvewx lanes as REX_LANE_U32, so
        # the quads are matched in the indexed form here and the lanes written back after (the round trip is exact:
        # both forms read the same lane; only bare vector names are taken, the only ones MS_QUAD matches).
        t = LANE_FORM.sub(r'\1.u32[3 - ((ea & 0xF) >> 2)]', m.group(0))
        t = MS_PAIR.sub(pair, t)
        t = MS_QUAD.sub(quad, t)
        return IDX_FORM.sub(r'REX_LANE_U32(\1, 3 - ((ea & 0xF) >> 2))', t)

    text = F_FUNC.sub(func, text)
    return text, counts[0], counts[1]


def localize(index, hooks, reach, root, what, opts, out_dir, quiet=False, hand=None):
    funcs = index.funcs
    hand = hand or {}
    if root not in funcs:
        sys.exit(f'localize_native: {root} is not a generated function')
    why = fits(index, hooks, root)
    if why:
        sys.exit(f'localize_native: {root} cannot be a copy: {why}')
    if hooks.replaced(root):
        sys.exit(f'localize_native: {root} is mentioned by an app or SDK source: a hook may replace it already')
    max_lines = int(opts.get('closure', 6000))
    members, left_out = closure_of(index, hooks, root, max_lines, int(opts.get('member', 1200)))
    member_set = set(members)
    externs = set()
    stats = {'calls_closed': 0, 'calls_open': 0, 'synced': []}

    def call_reach(m):
        """(what the callee and everything under it mention, what they can change, is it a copy of this header)."""
        written_name = m.group(1)
        b = base_name(written_name) if written_name else None
        if b in member_set:
            return reach.of(b) + (True,)
        if written_name is not None:
            return reach.of(written_name) + (False,)
        if m.group(0).startswith('ppc_trap('):
            return {'r3', 'r4'}, set(), False  # the trap helper reads two registers and writes none
        return ALL, ALL, False

    sync_memo = {}

    def call_syncs(name):
        """blur-nx (2026-11-16, CG-11): can a call of this function (or anything under it) synchronise with another
        thread? True when unsure (a cycle while it is being decided, a function outside the index)."""
        b = base_name(name) or name
        if b in sync_memo:
            return sync_memo[b]
        sync_memo[b] = True
        f = index.funcs.get(b)
        if f is None or f['unknown'] or hooks.replaced(b) or name.startswith('__imp__') or (hand and b in hand):
            return True
        if any(s.startswith('host function') or 'setjmp' in s or 'longjmp' in s for s in f['special']):
            return True
        for _, l in index.body(b):
            if ATOMIC_LOAD.match(l) or ATOMIC_CAS.match(l) or ATOMIC_FENCE.match(l) or 'storeFromGuest' in l or \
                    '__imp__' in l or 'REX_CALL_INDIRECT' in l or 'ppc_trap(' in l:
                return True
        result = any(call_syncs(c) for c in f['callees'])
        sync_memo[b] = result
        return result

    def call_text(m, me, my_regs, my_written, localized, site=None):
        """The replacement of one call statement. my_written: the registers that can be dirty here. site: the guest
        instruction index of the statement in its function (tools/reg_liveness.py's call sites), when known."""
        written_name = m.group(1)
        b = base_name(written_name) if written_name else None
        mention, change, closed = call_reach(m)
        if not closed and written_name:
            externs.add(written_name)
        if not localized:
            if closed:
                return '{ o_%s(ctx, base, s); if (s.Stopped()) return; }' % b
            return '{ s.Open(); return; }'
        out_regs = my_written if mention is ALL else [r for r in my_written if r in mention]
        # sasrt-nx (2026-11-12, stage 1; port 121): a real call's hand-over without what the callee does not read before
        # writing it and nothing reads after the call (tools/reg_liveness.py; the callee must be the one it saw).
        if not closed and site is not None and written_name:
            mm = re.match(r'^(?:__imp__)?sub_([0-9A-F]{8})$', written_name)
            fm = re.match(r'^(?:__imp__)?sub_([0-9A-F]{8})$', me)
            row = reg_liveness()['sites'].get(int(fm.group(1), 16), {}).get(site) if (mm and fm) else None
            if row and row[0] == int(mm.group(1), 16):
                kept = [r for r in out_regs if reg_bit(r) is None or not (row[1] >> reg_bit(r) & 1)]
                if len(kept) < len(out_regs):
                    CALL_SKIPPED[0] += 1
                    CALL_SKIPPED[1] += len(out_regs) - len(kept)
                    out_regs = kept
        # sasrt-nx (2026-11-12, stage 1d; port 121): an indirect call (or a promoted call's fallback) without the scratch
        # vector registers dead after it (a function reached through a pointer reads none of them on entry).
        if not closed and site is not None and not written_name and m.group(0).startswith('REX_CALL_INDIRECT'):
            fm = re.match(r'^(?:__imp__)?sub_([0-9A-F]{8})$', me)
            mask = reg_liveness()['indirect'].get(int(fm.group(1), 16), {}).get(site) if fm else None
            if mask:
                kept = [r for r in out_regs if reg_bit(r) is None or not (mask >> reg_bit(r) & 1)]
                if len(kept) < len(out_regs):
                    CALL_SKIPPED[2] += 1
                    CALL_SKIPPED[3] += len(out_regs) - len(kept)
                    out_regs = kept
        in_regs = my_regs if change is ALL else [r for r in my_regs if r in change]
        # blur-nx (2026-11-17, #232 of docs/cpu-codegen-universal-30-1116.md, near-exact): after a real call, the
        # PowerPC ABI's non-volatile registers are taken as the callee left them, i.e. as they were: not reloaded. Only
        # a callee that breaks the ABI (hand-written assembly) tells; the guard then sees this copy's write-back of the
        # register differ from the original's. BLUR_LOCALIZE_ABI_TRUST=0: every register the callee can change, as before.
        if ABI_TRUST and not closed:
            kept_in = [r for r in in_regs if r not in NONVOLATILE]
            if len(kept_in) < len(in_regs):
                ABI_SKIPPED[0] += 1
                ABI_SKIPPED[1] += len(in_regs) - len(kept_in)
                in_regs = kept_in
        before = ''.join(f'ctx.{r} = l_{r}; ' for r in out_regs)
        after = ''.join(f' l_{r} = ctx.{r};' for r in in_regs)
        stats['synced'].append((len(out_regs), len(in_regs)))
        if closed:
            stats['calls_closed'] += 1
            if b in hand:
                # The hand-written native first; the generated copy of the callee when it declines. katamari-nx
                # (2026-10-30 evening): what the callee may change is loaded back after EITHER path. banjo-nx's
                # natives write no register (the load-back was only on the declined path), but Katamari's write the
                # registers their originals write back (hand.txt exact=: the hash and the comparison return r3), and
                # a caller that read r3 after an inlined native that ran got its own stale r3 (sub_82737F80 and
                # sub_8271F9C0 took the other branch; the first diet PC run crashed after them).
                return ('{ ' + before + CARD_FLUSH + 'if (!%s(ctx, base, s)) { f_%s(ctx, base, s); '
                        'if (s.Stopped()) return; } else if (s.Stopped()) return;%s }' % (native_call(hand[b]), b, after))
            # blur-nx (2026-11-16, the 15 ms batch, CG-11 of docs/codegen-ideas-3.md): the pending card run is flushed
            # before a call only when the callee's tree can tell another thread about stores (a fence, an atomic, a
            # kernel import, an indirect or unknown call, a hook). Otherwise this copy's next flush (at its own
            # synchronisation or return) marks the page in time for Renut, which reads the cards only after one.
            flush = CARD_FLUSH if call_syncs(b) else ''
            return '{ ' + before + flush + 'f_%s(ctx, base, s); if (s.Stopped()) return;' % b + after + ' }'
        stats['calls_open'] += 1
        return '{ if (s.Open()) return; ' + before + CARD_FLUSH + m.group(0) + after + ' }'

    # Callee copies the compiler must inline into their callers: then a guest call inside the copy is no call at all,
    # the registers handed over through the context are forwarded (the context is `restrict`: the store before the
    # call and the load in the callee are the same value) and constant arguments reach the callee's code. Not a
    # function that can call itself, and not a large one with several call sites (the copy would only grow).
    calls_to = {m: 0 for m in members}
    callees_of = {}
    for name in members:
        callees_of[name] = set()
        for l in (text for _, text in index.body(name)):
            for m in CALL.finditer(l):
                b = base_name(m.group(1)) if m.group(1) else None
                if b in member_set:
                    calls_to[b] += 1
                    callees_of[name].add(b)

    def reaches_itself(name):
        seen, stack = set(), list(callees_of[name])
        while stack:
            n = stack.pop()
            if n == name:
                return True
            if n not in seen:
                seen.add(n)
                stack.extend(callees_of[n])
        return False

    inline_limit = int(opts.get('inline', 300))
    forced = {m for m in members[1:] if not reaches_itself(m) and
              (funcs[m]['lines'] <= inline_limit or calls_to[m] == 1) and inline_limit > 0}
    # The generated copy of a function with a hand-written native only runs when the native declines: out of line and
    # cold (inlined, it took registers away from the hand-written path: every call then saved five pairs of them).
    forced -= set(hand)
    # sasrt-nx (2026-11-11, port 74; its idea 34): a constant argument reaches a callee only when the compiler inlines
    # it (IPA-CP cannot follow the context: sasrt-nx's batch NRO had 1 .constprop clone among the copies). The members
    # forced inline above took 5,413 of sasrt-nx's 9,412 calls between copies; of the others, those called with a literal
    # in an argument register (set in the same block before the call, a register the callee reads, not written again
    # before the call) are forced inline too when at most 3 calls reach them and they have at most 4x the inline limit's
    # lines: their inlined body is the constant-argument clone. Only an inlining decision (the statements are the same).
    # SASRT_LOCALIZE_CONST_ARGS=0: off; inline=0 in the list: off for that copy.
    if CONST_ARGS and inline_limit > 0:
        literal = re.compile(r'^\tctx\.(r\d+)\.s64 = -?\d+;$')
        for name in members:
            ls = [l for _, l in index.body(name)]
            for i, l in enumerate(ls):
                for m in re.finditer(r'\b(?:__imp__)?(sub_[0-9A-F]{8})\(ctx, base\);', l):
                    b = m.group(1)
                    if (b not in members[1:] or b in forced or b in hand or reaches_itself(b) or calls_to[b] > 3
                            or funcs[b]['lines'] > 4 * inline_limit):
                        continue
                    for k in range(i - 1, max(-1, i - 9), -1):
                        if LOOP_LABEL.match(ls[k]):
                            break
                        mc = literal.match(ls[k])
                        if mc and mc.group(1) in funcs[b]['regs'] and not any(
                                re.search(r'\bctx\.' + mc.group(1) + r'\.\w+ = ', ls[q]) for q in range(k + 1, i)):
                            forced.add(b)
                            CONST_ARG_FORCED[0] += 1
                            break

    declarations = []
    dead_out = set()  # port 74, its idea 37: (member, register) write-backs LiveOut leaves out
    # Blocks: straight runs of statements. A block starts at a function's first statement, at every label and after
    # every statement that can leave (a jump, a return, the end of a switch). The Counting policy counts their
    # entries; the map says which lines of this header and of the generated file each block holds.
    blocks = 0
    entry_block = {}
    block_of_line = []  # (text, block or None) for every line of the function bodies, in order
    original_lines = {}
    for name in members:
        numbered = index.body(name)
        lines = [l for _, l in numbered]
        text = '\n'.join(lines)
        regs = funcs[name]['regs']
        written = funcs[name]['written']
        keep_out = live_out_keep(name)  # port 74, its idea 37
        if keep_out is not None:
            dead_out.update((name, r) for r in written if not keep_out(r))
        for m in PROMOTED.finditer(text):
            externs.add(m.group(1))
        if name in PREFETCH_RULES:  # blur-nx (2026-11-13, M-19): the rule's labels must still be there
            have = {l.strip()[:-1] for l in lines if LABEL.match(l)}
            gone = sorted(set(PREFETCH_RULES[name]) - have)
            if gone:
                sys.exit(f'localize_native: {name}: prefetch_rules.txt names {", ".join(gone)}, which its code no '
                         f'longer has (the guest code under the rule changed: check the rule)')
        dirty, writes, dirty_end = dirty_sets(name, lines, lambda l: [call_reach(m)[0] for m in CALL.finditer(l)])
        # sasrt-nx (2026-11-12, stage 1; port 121): the guest instruction index of each generated line (reg_liveness.py)
        raw = index.files.get(funcs[name]['file']) or []
        a0 = funcs[name]['at'][0]
        node_at = reg_liveness_mod.node_of_lines(raw[a0:funcs[name]['at'][1]]) if CALL_LIVENESS and raw else []
        in_loop, counted_barriers = loop_info(lines)  # port 74, its ideas 52 and 60
        file_lines = original_lines.setdefault(funcs[name]['file'], {})
        for localized in (True, False):
            body = []  # (text, block)
            block = None
            if localized:
                block = blocks
                blocks += 1
                entry_block[name] = block
                # Everything of the function in the generated file that is not a statement below (the leaf-local
                # declarations and final stores, the prologue) counts with its first block.
                for k in range(funcs[name]['at'][0] - 1, funcs[name]['at'][1] + 1):
                    file_lines[k + 1] = block
                body.append((f'\ts.Hit({block});', block))
            pending = False
            in_switch = False
            for at, (number, l) in enumerate(numbered):
                # What can be dirty when this statement's call or return runs (an unreached statement: everything).
                here = sorted(written if dirty[at] is None else (dirty[at] | writes[at]), key=order)
                sync_out = ''.join(f'ctx.{r} = l_{r}; ' for r in here)
                # (Port 74, its idea 37: a return writes back only what a caller can read; a call still gets `here`.)
                sync_ret = (sync_out if keep_out is None else
                            ''.join(f'ctx.{r} = l_{r}; ' for r in here if keep_out(r)))
                if localized:
                    if LABEL.match(l):
                        block = blocks
                        blocks += 1
                        pending = False
                        body.append((l, block))
                        body.append((f'\ts.Hit({block});', block))
                        for stmt in PREFETCH_RULES.get(name, {}).get(l.strip()[:-1], ()):
                            body.append(('\t' + REG.sub(lambda m: 'l_' + m.group(1), stmt), block))  # M-19
                            LOOP_STATS[2] += 1
                        file_lines[number] = block
                        continue
                    if pending:
                        block = blocks
                        blocks += 1
                        pending = False
                        body.append((f'\ts.Hit({block});', block))
                    file_lines[number] = block
                    if l.startswith('\tswitch ('):
                        in_switch = True
                    elif in_switch and l == '\t}':
                        in_switch = False
                        pending = True
                    if not in_switch and (GOTO.search(l) or RETURN.search(l)):
                        pending = True
                    l = REG.sub(lambda m: 'l_' + m.group(1), l)
                if localized and at in counted_barriers:
                    LOOP_STATS[1] += 1  # port 74, its idea 60: a straight counted loop's barrier left out of the running form
                    continue
                l, stores = store_line(l)
                l = atomic_stop(l)  # sasrt-nx (2026-11-10): see ATOMICS
                site = node_at[number - 1 - a0] if 0 <= number - 1 - a0 < len(node_at) else None  # port 121
                l = CALL.sub(lambda m: call_text(m, name, regs, here, localized, site), l)
                if localized and (sync_ret or CARD_FLUSH):
                    # Braces: the generated code also writes a return as the body of a one-line `if`. (Port 74: only
                    # the live write-backs, its idea 37, and the pending card run flushed, its idea 39.)
                    l = RETURN.sub(lambda m: m.group(0) if 'if (s.' in l[max(0, m.start() - 24):m.start()]
                                   else '{ ' + sync_ret + CARD_FLUSH + 'return; }', l)
                body.append((l, block))
                if stores:
                    body.append(('\tif (s.Stopped()) return;', block))
                if localized and PREFETCH and at in in_loop:
                    mc = CHASE.match(l)
                    if mc:
                        body.append((f'\tREX_PREFETCH_GUEST({mc.group(1)}.u32);', block))  # port 74, its idea 52
                        LOOP_STATS[0] += 1
            if localized:
                how = ('[[gnu::always_inline]] static inline' if name in forced
                       else '[[gnu::noinline, gnu::cold]] static' if name in hand else 'static')
                head = (f'template <class S>\n{how} '
                        f'void f_{name}(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] S& s)')
                declarations.append(head + ';')
                block_of_line.append((head.split('\n')[0], None))
                block_of_line.append((head.split('\n')[1] + ' {', entry_block[name]))
                if CARD_RUNS:
                    block_of_line.append(('\tREX_CARD_RUN_DECL', entry_block[name]))  # port 74, its idea 39
                for r in regs:
                    block_of_line.append((f'\t{type_of(r)} l_{r} = ctx.{r};', entry_block[name]))
                block_of_line.extend(body)
                end = sorted(written if dirty_end is None else dirty_end, key=order)
                if keep_out is not None:
                    end = [r for r in end if keep_out(r)]  # port 74, its idea 37
                if end and not (numbered and ONLY_RETURN.match(numbered[-1][1])):
                    block_of_line.append(('\t' + ' '.join(f'ctx.{r} = l_{r};' for r in end), block))
                if CARD_RUNS and not (numbered and ONLY_RETURN.match(numbered[-1][1])):
                    block_of_line.append(('\tREX_CARD_FLUSH();', block))  # port 74, its idea 39: the function's end
                block_of_line.append(('}', block))
                block_of_line.append(('', None))
            else:
                head = (f'static void o_{name}(PPCContext& __restrict ctx, uint8_t* base, '
                        f'[[maybe_unused]] ::banjo::hot::Logged& s)')
                declarations.append(head + ';')
                block_of_line.append((head + ' {', None))
                if CARD_RUNS:
                    # Port 74, its idea 39: the shard's REX_CARD needs the run local here too. This form only runs in
                    # the guard's rolled-back pass, so its last run's page staying unmarked does not matter.
                    block_of_line.append(('\tREX_CARD_RUN_DECL', None))
                block_of_line.extend((l, None) for l, _ in body)
                block_of_line.append(('}', None))
                block_of_line.append(('', None))

    addr = addresses(index.gen)
    image = open(IMAGE, 'rb').read() if os.path.isfile(IMAGE) else None
    if image is None:
        sys.exit('localize_native: out/renut_image.bin is missing (the code generator writes it: tools\\pc_env.bat)')
    ranges = []
    h = 0xCBF29CE484222325
    for name in members:
        a, n = addr[name]
        ranges.append((a, n))
        h = fnv1a64(image[a - IMAGE_BASE:a - IMAGE_BASE + n], h)
    rid = identifier(root)
    callees = [m for m in members[1:]]
    # The registers the hand-written natives in this header leave as they found them where the original changes them.
    hand_members = [m for m in members if m in hand]
    skip = set()
    for m in hand_members:
        if not hand[m].get('inexact'):
            skip |= set(funcs[m]['written']) - hand[m]['exact']
    # sasrt-nx (2026-11-11, port 74; its idea 37): the registers the hook may leave different from the original: those
    # dead after the root returns (its LiveOut). A member's dropped write-back of a register live after the root is
    # rewritten before the root's exit (or LiveOut would have kept it): still compared (sasrt-nx's round A audit,
    # finding 4).
    root_keep = live_out_keep(root)
    if root_keep is not None:
        skip |= {r for m in members for r in funcs[m]['written'] if not root_keep(r)}
    LIVE_OUT_DROPPED[0] += sum(1 for m, _ in dead_out if m in members)
    entry = (f'if (!{native_call(hand[root])}(ctx, base, s)) f_{root}(ctx, base, s);'
             if root in hand else f'f_{root}(ctx, base, s);')
    # blur-nx (2026-11-16): the guest calls of the natives that make any (hand.txt calls=), after the declarations.
    for m in hand_members:
        if hand[m].get('calls'):
            declarations.extend([''] + calls_struct(hand[m], member_set, externs, hand))
    hand_note = ('' if not hand_members else
                 '\n// Hand-written natives in front of their copies (app/src/cpu/hot/banjo_hand_natives.h): '
                 + ', '.join(hand_members) + '.\n// Registers the guard does not compare for this hook (they leave '
                 'them as they found them): ' + (', '.join(sorted(skip, key=order)) or 'none') + '.')
    left = ''.join(f'\n//   {k}: {v}' for k, v in sorted(left_out.items()))
    synced = ', '.join(f'{a}/{b}' for a, b in stats['synced'])
    top = f'''// {root}: {what or "localized copy"}.
// GENERATED by tools/localize_native.py from app/generated/{funcs[root]["file"]} (do not edit; tools/post_codegen.py
// writes it again after every codegen). The recompiled function's own statements with its {len(funcs[root]["regs"])} context registers
// in C++ locals ({len(funcs[root]["written"])} of them written back at a return), {len(callees)} callee(s) as copies of their own{": " + ", ".join(callees) if callees else ""}.
// Calls: {stats["calls_closed"]} to copies, {stats["calls_open"]} real (registers stored before / loaded after each: {synced or "-"}).
// Real calls, which a guarded call cannot repeat:{left or " none"}
// f_*<Fast> runs; f_*<Logged> and o_* (the original statements) record their guest stores for the guard
// (app/src/cpu/banjo_hot_guest.cpp); f_*<Counting> counts block entries for tools/hot_arm_compare.py.{hand_note}
// Mechanism from sa2-nx (GPL-3.0).
#pragma once
#ifndef BANJO_HOT_NATIVE_SPEC  // port 81: app/src/cpu/banjo_hot_guest.h's (SASRT_HOT_IN_PLACE), when included first
#define BANJO_HOT_NATIVE_SPEC [[gnu::noinline]]
#endif

{"".join(f"REX_EXTERN({c});" + chr(10) for c in sorted(externs))}
namespace banjo::hot::n_{rid} {{

{chr(10).join(declarations)}
'''.split('\n')
    bottom = f'''// The guest code these copies were made from (first call: the hook stays off unless it is loaded unchanged).
inline constexpr ::banjo::hot::CodeRange kCode[] = {{{", ".join(f"{{0x{a:08X}u, {n}u}}" for a, n in ranges)}}};
inline constexpr uint64_t kCodeHash = 0x{h:016X}ull;
inline std::atomic<uint64_t> g_hits[{blocks}];  // banjo_hot_count
inline constexpr ::banjo::hot::RegSkip kSkip = {skip_literal(skip)};

BANJO_HOT_NATIVE_SPEC inline void Native(PPCContext& ctx, uint8_t* base) {{
  ::banjo::hot::Fast s;
  {entry}
}}
inline void NativeLogged(PPCContext& ctx, uint8_t* base, ::banjo::hot::Logged& s) {{ {entry} }}
inline void ReferenceLogged(PPCContext& ctx, uint8_t* base, ::banjo::hot::Logged& s) {{ o_{root}(ctx, base, s); }}
inline void NativeCounted(PPCContext& ctx, uint8_t* base) {{
  ::banjo::hot::Counting s{{g_hits}};
  {entry}
}}

}}  // namespace banjo::hot::n_{rid}
'''.split('\n')
    header_lines = {}
    for k, (_, block) in enumerate(block_of_line):
        if block is not None:
            header_lines[len(top) + k + 1] = block
    for k, l in enumerate(bottom):  # the entry function: with the root's first block
        if l.startswith('BANJO_HOT_NATIVE_SPEC inline void Native('):  # port 81 (sasrt-nx's in-place copies)
            for j in range(4):
                header_lines[len(top) + len(block_of_line) + k + j + 1] = entry_block[root]
    header = '\n'.join(top + [l for l, _ in block_of_line] + bottom)
    # Port 74 (its idea 57): the merged vector stores (merge_vector_stores above). The line numbers of blocks.json
    # (tools/hot_arm_compare.py) are those before the merge.
    header, merged_pairs, merged_quads = merge_vector_stores(header)
    MERGED[0] += merged_pairs
    MERGED[1] += merged_quads
    if merged_pairs or merged_quads:
        MERGED[2] += 1
    path = os.path.join(out_dir, f'n_{rid}.h')
    os.makedirs(out_dir, exist_ok=True)
    try:
        same = open(path, encoding='utf-8').read() == header
    except OSError:
        same = False
    if not same:
        open(path, 'w', encoding='utf-8', newline='\n').write(header)
    json.dump({'root': root, 'id': rid, 'members': members, 'blocks': blocks, 'entry': entry_block,
               'header': header_lines, 'original': original_lines},
              open(os.path.join(out_dir, f'n_{rid}.blocks.json'), 'w', encoding='utf-8'))
    if not quiet:
        print(f'{root}: {len(funcs[root]["regs"])} registers, closure {len(members)} functions '
              f'({sum(funcs[m]["lines"] for m in members)} lines), calls {stats["calls_closed"]} to copies + '
              f'{stats["calls_open"]} real, {blocks} blocks -> {shown(path)}{"" if not same else " (unchanged)"}')
    if not quiet and hand_members:
        print(f'    hand-written natives in front: {", ".join(hand_members)}; skip {", ".join(sorted(skip, key=order)) or "-"}')
    return {'root': root, 'id': rid, 'members': members, 'left_out': left_out, 'stats': stats}


def key_of(root, opts):
    if opts.get('key'):
        return opts['key']
    return 'banjo_cpu_native_' + (root if root.startswith('sub_') else re.sub(r'^rex_', '', root).lower())[:56]


def write_list(entries, out_dir, bulk=()):
    """banjo_hot_all.h (the copies) and banjo_hot_list.inc (one BANJO_HOT_HOOK line each) for banjo_hot_guest.cpp;
    banjo_hot_bulk_<k>.inc (the bulk list's headers and BANJO_HOT_HOOK_BULK lines) for banjo_hot_shard_<k>.cpp."""
    # renut-ultimate (port 69, 2026-11-10, blur-nx): a fresh tree has no generated/hot yet (a new port's first PC build
    # stopped here).
    os.makedirs(out_dir, exist_ok=True)
    inc = ['// GENERATED by tools/localize_native.py from app/src/cpu/hot/localized.txt (do not edit).']
    allh = ['// GENERATED by tools/localize_native.py from app/src/cpu/hot/localized.txt (do not edit).', '#pragma once']
    for slot, (root, opts, what) in enumerate(entries):
        rid = identifier(root)
        allh.append(f'#include "n_{rid}.h"')
        desc = ('Native localized copy of %s: %s' % (root, what)).replace('\\', '/').replace('"', "'")
        inc.append(f'BANJO_HOT_HOOK({root}, {rid}, {key_of(root, opts)}, {"false" if opts.get("off") else "true"}, '
                   f'{slot}, "{desc}")')
    # The bulk list over the shards: the largest headers first, each to the shard with the fewest bytes so far.
    shards = [[0, []] for _ in range(SHARDS)]
    for root, opts, what in sorted(bulk, key=lambda e: -os.path.getsize(os.path.join(out_dir, f'n_{identifier(e[0])}.h'))):
        rid = identifier(root)
        shard = min(shards, key=lambda s: s[0])
        shard[0] += os.path.getsize(os.path.join(out_dir, f'n_{rid}.h'))
        desc = ('Bulk localized copy of %s: %s' % (root, what)).replace('\\', '/').replace('"', "'")
        shard[1] += [f'#include "n_{rid}.h"',
                     f'BANJO_HOT_HOOK_BULK({root}, {rid}, {key_of(root, opts)}, {"false" if opts.get("off") else "true"}, '
                     f'{BULK_SLOT}, "{desc}")']
    for k, (size, lines) in enumerate(shards):
        text = '\n'.join([f'// GENERATED by tools/localize_native.py --bulk from app/src/cpu/hot/localized_bulk.txt (do not '
                          f'edit): shard {k} of {SHARDS},', f'// {size // 1024} KB of copies, compiled by '
                          f'app/src/cpu/banjo_hot_shard_{k}.cpp.'] + lines) + '\n'
        path = os.path.join(out_dir, f'banjo_hot_bulk_{k}.inc')
        # blur-nx (2026-11-10): a fresh tree has no generated/hot yet (the first build of a new port stopped here).
        os.makedirs(out_dir, exist_ok=True)
        try:
            same = open(path, encoding='utf-8').read() == text
        except OSError:
            same = False
        if not same:
            open(path, 'w', encoding='utf-8', newline='\n').write(text)
    # Headers of functions no longer in the lists (this tool's own output) go.
    keep = {f'n_{identifier(root)}' for root, _, _ in list(entries) + list(bulk)}
    if os.path.isdir(out_dir):
        for fn in os.listdir(out_dir):
            stem = fn[:-len('.blocks.json')] if fn.endswith('.blocks.json') else fn[:-2] if fn.endswith('.h') else None
            if stem and stem.startswith('n_') and stem not in keep:
                os.remove(os.path.join(out_dir, fn))
    for name, lines in (('banjo_hot_all.h', allh), ('banjo_hot_list.inc', inc)):
        path = os.path.join(out_dir, name)
        text = '\n'.join(lines) + '\n'
        try:
            same = open(path, encoding='utf-8').read() == text
        except OSError:
            same = False
        if not same:
            os.makedirs(out_dir, exist_ok=True)
            open(path, 'w', encoding='utf-8', newline='\n').write(text)


def survey(index, hooks, reach, names):
    for name in names:
        name = name if not re.fullmatch(r'[0-9A-Fa-f]{8}', name) else 'sub_' + name.upper()
        f = index.funcs.get(name)
        if f is None:
            print(f'{name}: not a generated function')
            continue
        why = fits(index, hooks, name)
        members, left_out = closure_of(index, hooks, name, 6000, 1200)
        opens = sorted(left_out.items())
        print(f'{name}: {f["lines"]} lines, {len(f["regs"])} ctx registers ({len(f["written"])} written), '
              f'{len(f["callees"])} direct callees, {f["unknown"]} indirect calls or traps '
              f'({f.get("promoted", 0)} of them promoted sites\' fallbacks)'
              f'{"; CANNOT be a copy: " + why if why else ""}'
              f'{"; replaced by an app hook already?" if hooks.replaced(name) else ""}')
        print(f'    closure: {len(members)} functions, {sum(index.funcs[m]["lines"] for m in members)} lines'
              + (': ' + ', '.join(members[1:]) if len(members) > 1 else ''))
        for k, v in opens:
            print(f'    real call {k}: {v}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('names', nargs='*')
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--out', default=OUT, help='folder for the headers (default app/generated/hot)')
    ap.add_argument('--list', nargs='?', const=LIST, help='localize every function of the list file')
    ap.add_argument('--bulk', nargs='?', const=BULK, help='with --list: the bulk list too (its hooks in shard files)')
    ap.add_argument('--survey', action='store_true', help='only say what a copy of each name would contain')
    ap.add_argument('--what', default='')
    a = ap.parse_args()
    entries = list_entries(a.list or LIST)
    index = Index(a.gen)
    hand = hand_entries()
    listed = {e[0] for e in entries}
    bulk = [e for e in list_entries(a.bulk) if e[0] not in listed and e[0] not in hand] if a.bulk else []
    # A bulk function is a native too: its copy touches no more than its original (and it is not an app hook).
    hooks = Hooks([e[0] for e in entries] + [e[0] for e in bulk])
    reach = Reach(index, hooks)
    for name in hand:
        if name not in index.funcs:
            sys.exit(f'localize_native: hand.txt: {name} is not a generated function')
    if a.survey:
        survey(index, hooks, reach, a.names)
        return
    if a.list:
        for root, opts, what in entries:
            localize(index, hooks, reach, root, what, opts, a.out, hand=hand)
        made, skipped = [], []
        for root, opts, what in bulk:
            try:
                localize(index, hooks, reach, root, what, opts, a.out, quiet=True, hand=hand)
                made.append((root, opts, what))
            except SystemExit as e:  # a form the tool refuses: that function stays generated code
                skipped.append(f'{root} ({str(e).replace("localize_native: ", "")[:90]})')
        write_list(entries, a.out, made)
        print(f'localize_native: {LIVE_OUT_DROPPED[0]} return write-backs left out by LiveOut (idea 37)')
        print(f'localize_native: {CALL_SKIPPED[1]} register stores left out of {CALL_SKIPPED[0]} real calls (their '
              f'hand-overs) (stage 1, tools/reg_liveness.py; data {"used" if reg_liveness()["sites"] else "NOT available"}); '
              f'{CALL_SKIPPED[3]} scratch vector stores left out of {CALL_SKIPPED[2]} indirect calls (stage 1d)')
        print(f'localize_native: {LOOP_STATS[0]} pointer-chase prefetches (idea 52), {LOOP_STATS[1]} counted loops\' '
              f'barriers left out (idea 60), {LOOP_STATS[2]} hand-named prefetches (M-19, prefetch_rules.txt)')
        print(f'localize_native: {CONST_ARG_FORCED[0]} members with a constant argument forced inline (idea 34)')
        print(f'localize_native: {ABI_SKIPPED[1]} non-volatile register reloads left out after {ABI_SKIPPED[0]} real '
              f'calls (#232, ABI trust{"" if ABI_TRUST else ": OFF"})')
        # Port 74 (its idea 39; sasrt-nx's batch 2 audit, finding 2): the shards turn the card runs on only for copies
        # written with them (app/src/cpu/banjo_hot_prelude.h reads this header).
        opt = os.path.join(a.out, 'banjo_hot_options.h')
        text = ('// GENERATED by tools/localize_native.py: how the copies were written (app/src/cpu/banjo_hot_prelude.h).\n'
                '#pragma once\n' + ('#define REX_CARD_RUNS_GENERATED 1\n' if CARD_RUNS else '')
                # sasrt-nx (2026-11-11, port 81; machine copies in place): the copies' entry is
                # BANJO_HOT_NATIVE_SPEC (app/src/cpu/banjo_hot_guest.h tells an in-place build of older headers).
                + '#define BANJO_HOT_NATIVE_SPEC_HEADERS 1\n')
        if not os.path.exists(opt) or open(opt, encoding='utf-8').read() != text:
            with open(opt, 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
        print(f'localize_native: merged vector stores: {MERGED[0]} stvlx/stvrx pairs and {MERGED[1]} stvewx quads in '
              f'{MERGED[2]} copies')
        print(f'localize_native: {len(entries)} copies in {shown(a.out)}'
              + (f'; bulk: {len(made)} copies in {SHARDS} shards' if a.bulk else ''))
        if skipped:
            print(f'localize_native: bulk: {len(skipped)} left out:\n    ' + '\n    '.join(skipped))
        return
    if not a.names:
        ap.error('names, --list or --survey')
    for name in a.names:
        name = name if not re.fullmatch(r'[0-9A-Fa-f]{8}', name) else 'sub_' + name.upper()
        hooks.natives.add(name)
        localize(index, hooks, reach, name, a.what, {}, a.out, hand=hand)


if __name__ == '__main__':
    main()
