#!/usr/bin/env python3
"""banjo-nx: the post-codegen steps of the optimised codegen (from sa2-nx's tools/post_codegen.py; docs/enhancements.md,
"Build-time"). Run after EVERY codegen: app/CMakeLists.txt appends this script to the generated renut_codegen command,
because the PC build re-runs the generator by itself whenever an input changes (renut_config.toml, the xex, the
generator) and that would otherwise drop the steps silently. Banjo ships only the optimised codegen (one NRO).

The sequence is sa2-nx's (its three CPU rounds of 2026-10-12, docs/enhancements.md "sa2-nx's CPU rounds"), on Banjo's one
flat tree (app/generated), with its localized native copies as the last step (2026-10-13):

  - tools/pch_no_volatile.py + tools/loop_barriers.py (required together: without barriers GCC turns guest polling
    loops into infinite loops);
  - tools/indirect_calls.py: every `bctrl` tagged with its site (PC census) and the sites of
    app/indirect_promotions.txt as guarded direct calls. Before loop_barriers.py (a loop needs no barrier when every
    way round it passes an indirect call, and a promoted call is no longer one) and before direct_calls.py;
  - tools/pch_cold_traps.py: the pch's trap helper out of line and cold;
  - tools/direct_calls.py --redo (calls between unhooked functions become direct, so GCC can inline them);
  - sasrt-nx's steps: tools/guest_cas.py (compare-and-swap without the legacy builtin's trailing `dmb ish` on
    AArch64), tools/flush_mode_flow.py (flush-mode checks the control flow and the callees fix: nothing on the Switch,
    still tested in the PC build; after direct_calls.py, whose --redo can turn a direct call back into a hooked one),
    tools/vec_byteswap.py (guest vector byte reversal without a table lookup on AArch64), tools/pch_neon_shuffles.py;
  - tools/ro_constants.py (loads from the image's read-only data as constants, the XEX's import records excepted
    with --xex; before float_loads.py),
    tools/float_loads.py (a single-precision load straight into a float register), tools/frame_traffic.py (only marks
    prologue statements: its elisions are opt-in and OFF, sa2-nx found them not exact);
  - tools/loop_locals.py (the ctx registers of a call-free loop as locals for the length of the loop), then
    tools/leaf_locals.py (the ctx registers of leaf functions with loops; it renames registers);
  - from sasrt-nx's round of 2026-10-14: tools/fp_sign_ops.py, tools/leaf_dead_stores.py, tools/single_floats.py,
    tools/vperm_one_source.py (a vperm of one vector as a one-register lookup;
    simde_mm_perm1_epi8_ in sdk/include/rex/ppc/intrinsics.h, sdk/tests/vmx_helpers_neon_test.cpp),
    tools/inline_memcalls.py (memcpy / memset of a constant size up to 64 bytes inline; sdk/include/rex/ppc/memcalls.h,
    sdk/tests/memcalls_test.cpp), tools/vec_unaligned_stores.py (stvlx / stvrx as a few stores instead of byte
    loops; sdk/include/rex/ppc/vec_unaligned.h, sdk/tests/vec_unaligned_test.cpp);
  - tools/ro_constants_cfg.py (2026-10-16): the read-only constants over the control-flow graph (after a label);
  - tools/localize_native.py --list: the localized native copies of the hot functions of
    app/src/cpu/hot/localized.txt, into app/generated/hot (app/src/cpu/banjo_hot_guest.cpp includes them);
  - tools/store_cards.py (2026-10-19): every guest store outside the stack also marks its 4 KB page's store card, last
    of all (Renut proves its cached vertex and index copies by the cards; REX_STORE_CARDS_OFF compiles them out);
  - app/generated/codegen_flavor.txt (the build's name in the "[enhancements]" log line).

ro_constants.py and the entry modes of flush_mode_flow.py need the generator's image dump, out/renut_image.bin: the
generator writes it when REXGLUE_DUMP_IMAGE names it (tools/pc_env.bat sets that for every PC build).

Every step is idempotent, and so is the sequence (a second run on a processed tree changes no file).

  python tools/post_codegen.py [--gen <generated folder>]
"""
import os
import re
import subprocess
import sys


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
IMAGE = os.path.join(ROOT, 'out', 'renut_image.bin')
PROMOTIONS = os.path.join(APP, 'indirect_promotions.txt')
# Where a function's address can be written down outside the generated code (a hook, a host call of the original, a
# named function): the app's and the SDK's sources, and the tomls. NOT config/renut_funcs.toml: it lists every
# function boundary of the game, which would make every function look hooked.
HOOK_TOMLS = [APP, os.path.join(APP, 'config', 'renut_hooks.toml'), os.path.join(APP, 'config', 'renut_gpu_funcs.toml'),
              os.path.join(APP, 'config', 'renut_crt.toml')]


def run(*args):
    result = subprocess.run([sys.executable, *args], cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f'post_codegen: {os.path.basename(args[0])} failed ({result.returncode})')


def tool(name):
    return os.path.join(ROOT, 'tools', name)


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    scratch = os.path.abspath(gen) != os.path.abspath(GEN)  # a copy for measurements: no flavour file
    pch = gen_file(gen, 'pch.h')
    if not os.path.isfile(IMAGE):
        sys.exit('post_codegen: out/renut_image.bin is missing (the image dump of the code generator): run the PC '
                 'build through tools\\pc_env.bat, which sets REXGLUE_DUMP_IMAGE, or set it for one codegen run')
    xex = os.path.join(APP, 'assets', 'default.xex')
    # banjo-nx (2026-10-29): and the title update's patch next to it (the image is the patched one).
    xexp = xex + 'p'
    if os.path.isfile(xexp) and os.path.getmtime(xexp) > os.path.getmtime(IMAGE):
        sys.exit('post_codegen: out/renut_image.bin is older than app/assets/default.xexp: run the code generator with '
                 'REXGLUE_DUMP_IMAGE set (tools\\pc_env.bat does) so it writes the dump of the patched image again')
    if os.path.isfile(xex) and os.path.getmtime(xex) > os.path.getmtime(IMAGE):
        sys.exit('post_codegen: out/renut_image.bin is older than app/assets/default.xex: run the code generator with '
                 'REXGLUE_DUMP_IMAGE set (tools\\pc_env.bat does) so it writes the dump again')
    os.environ.setdefault('REXSDK_DIR', os.path.join(ROOT, 'sdk'))
    # cpu_aggressive (2026-10-16): the late rewrites below change statements the analyses parse (r1l for ctx.r1,
    # REX_LR_WRITE_SAME, computed gotos, hoisted checks): on a tree that was processed before they are taken back out
    # first, so every analysis sees the text it was written for. On a fresh codegen these find nothing.
    # blur-nx (2026-11-11, CPU batch 1): the DC ZVA pairs come out before the cards (store_cards.py finds a dcbz by its
    # memset, and tools/localize_native.py knows the memset's store, not REX_DCBZ128).
    run(tool('vec_dcbz_zva.py'), '--gen', gen, '--restore')
    # banjo-nx (2026-10-19): the store cards were the last step: out first.
    run(tool('store_cards.py'), '--gen', gen, '--restore')
    run(tool('switch_goto.py'), '--gen', gen, '--restore')
    run(tool('r1_local.py'), '--gen', gen, '--restore')
    run(tool('lr_preserve.py'), '--gen', gen, '--out', os.path.join(ROOT, 'out', 'cpu_aggressive_proofs'), '--restore')
    run(tool('flush_hoist.py'), '--gen', gen, '--restore')
    # Package 27's codegen round: the inline copies of tiny leaves and the hot declarations come out first too (the
    # analyses read calls by name and functions by their definition line).
    run(tool('inline_leaves.py'), '--gen', gen, '--restore')
    run(tool('vsel_neon.py'), '--gen', gen, '--restore')
    run(tool('hot_layout.py'), '--gen', gen, '--restore')
    # sasrt-nx (2026-11-12, its idea 2; port 121): the forwarded stack-slot vector loads out too (put back below).
    run(tool('stack_forward.py'), '--gen', gen, '--restore')
    # renut-ultimate (2026-11-16, port 144): the negated fused multiply-adds' macro out too (single_floats.py and
    # fma_single.py read the forms it wraps; put back below).
    run(tool('fnm_sign.py'), '--gen', gen, '--restore')
    run(tool('pch_no_volatile.py'), pch)
    run(tool('indirect_calls.py'), '--gen', gen, '--promote', PROMOTIONS)
    # REQUIRED with pch_no_volatile.py: without barriers GCC turns guest polling loops into infinite loops.
    run(tool('loop_barriers.py'), gen)
    run(tool('pch_cold_traps.py'), pch)
    # Undo and redo in one pass: a function hooked since the last run (a new native) must be called through its hook
    # again. --redo writes only the files that change, so the build does not recompile the others.
    run(tool('direct_calls.py'), '--redo', '--gen', gen)
    run(tool('guest_cas.py'), '--gen', gen)
    # blur-nx (2026-11-16, the 15 ms batch, #164 / #165): sync, lwsync and eieio with the PowerPC's orderings (lwsync
    # `dmb ishld` + `dmb ishst`, eieio `dmb ishst` on AArch64; g_rex_guest_fence_full puts the full fence back).
    run(tool('guest_fences.py'), '--gen', gen)
    run(tool('flush_mode_flow.py'), '--redo', '--gen', gen, '--image', IMAGE, '--tomls', *HOOK_TOMLS)
    run(tool('vec_byteswap.py'), '--gen', gen)
    run(tool('pch_neon_shuffles.py'), pch)
    # --xex: loads of the XEX's import records (kernel variable addresses the loader writes) stay loads (from
    # peggle2-nx, 2026-10-15; found by futari-nx: 77 such loads were folded here).
    xex = os.path.join(ROOT, 'app', 'assets', 'default.xex')
    run(tool('ro_constants.py'), '--gen', gen, '--image', IMAGE, *(['--xex', xex] if os.path.isfile(xex) else []))
    run(tool('float_loads.py'), '--gen', gen)
    run(tool('frame_traffic.py'), '--gen', gen)
    run(tool('loop_locals.py'), '--gen', gen)
    run(tool('leaf_locals.py'), '--gen', gen)
    # From sasrt-nx's codegen round of 2026-10-14 (each in its own tool, tested in sdk/tests), in its order.
    run(tool('vperm_one_source.py'), '--gen', gen)
    # fp_sign_ops: exact while the game never selects round-up / round-down (banjo_diag_runtime_census, 2026-10-14:
    # 24 guest FPSCR writes to nearest, 10 toward zero, 0 up or down in 345 s to the level).
    run(tool('fp_sign_ops.py'), '--gen', gen)
    run(tool('leaf_dead_stores.py'), '--gen', gen, '--image', IMAGE)
    run(tool('single_floats.py'), '--gen', gen)
    run(tool('inline_memcalls.py'), '--gen', gen)
    run(tool('vec_unaligned_stores.py'), '--gen', gen)
    # blur-nx (2026-11-11): stvewx / stvehx lanes read from the register, not stored to the stack and loaded back (the
    # A57 cannot forward the wider store; sdk/include/rex/ppc/vec_lane.h, checked on the device by the forms
    # self-check). Before the localized copies, so they take the same form.
    run(tool('vec_lane_index.py'), '--gen', gen)
    # blur-nx (2026-11-11, CPU batch 1; the user: "do all the exact ones", "optimise the hell out of CPU ... in large
    # batches"): docs/vector-store-ideas.md VS-27, VS-08, VS-25 and docs/vector-ideas-2.md VX-20, VX-21, each exact and
    # checked by the app's forms self-check on the device. Before the localized copies, so they take the same forms.
    run(tool('vec_cvt_fixed.py'), '--gen', gen)      # vcfsx / vcfux with a scale: one SCVTF / UCVTF #n
    run(tool('vec_prefetch_line.py'), '--gen', gen)  # a guest dcbt prefetches the whole 128-byte line
    run(tool('vec_lvlx_splat.py'), '--gen', gen)     # lvlx + vspltw 0: one 4-byte load (where the lvlx is dead after)
    run(tool('vec_shift_whole.py'), '--gen', gen)    # vsrw / vsraw as one vector shift; vslw without the dead clamp
    # banjo-nx (2026-10-16, CPU codegen agent): the read-only constants ro_constants.py could not see because a label
    # stood between the base register's constant and the load (loop heads): followed over the control-flow graph.
    run(tool('ro_constants_cfg.py'), '--gen', gen, '--image', IMAGE, *(['--xex', xex] if os.path.isfile(xex) else []))
    # banjo-nx (2026-10-15): guest register writes no instruction can read, on leaf_dead_stores.py's analysis; before
    # the localized copies, so each copy is made from the same statements as its function.
    # On since 2026-10-15 (the eighteenth package), after its crash was found (the parser missed the mid-asm hooks'
    # conditional gotos and register arguments) and the PC intro route ran clean with the poison build.
    # BANJO_DEAD_DEFS_OFF=1 takes any earlier run's rewrites back out instead (--restore-only).
    if os.environ.get('BANJO_DEAD_DEFS_HOLD'):  # tools/dead_defs_bisect.py has set the files: leave them
        print('post_codegen: dead_ctx_defs.py held (BANJO_DEAD_DEFS_HOLD)')
    elif os.environ.get('BANJO_DEAD_DEFS_OFF'):
        run(tool('dead_ctx_defs.py'), '--gen', gen, '--restore-only')
    else:
        run(tool('dead_ctx_defs.py'), '--gen', gen, '--image', IMAGE,
            *([] if os.environ.get('BANJO_DEAD_DEFS_NO_ABI') else ['--abi-indirect']))
    # cpu_aggressive A: a loop header's flush-mode check in front of the loop (178 loops; the copies below get it too).
    # Switch back: BANJO_GEN_FLUSH_HOIST=OFF (REX_FLUSH_HOIST_OFF: the original check in the loop again).
    run(tool('flush_hoist.py'), '--gen', gen)
    # sasrt-nx (2026-11-11, port 73; its machine-copy idea 43 B3): vupkd3d128 type 0 as one NEON table lookup (before
    # the copies, so they get it too; REX_VUPKD3D_LANES: the four lane lines).
    run(tool('vupkd3d_neon.py'), '--gen', gen)
    # sasrt-nx (2026-11-11, port 72; its machine-copy idea 43 F1-F4): vrsqrtefp, vrefp, vlogefp, vmaddfp and vnmsubfp as
    # macros (their default is the expression the generator writes; CMake SASRT_VMX_FAST_ESTIMATES / SASRT_VMX_FUSED_MADD,
    # ON, give the faster AArch64 forms). Before the copies, so they get the macros too.
    run(tool('vmx_float_macros.py'), '--gen', gen)
    # blur-nx (2026-11-12, the user: "Please do the fmadds change"): fmadds / fmsubs / fnmadds / fnmsubs rounded once in
    # single precision, as the Xenon does (sdk/include/rex/ppc/fma_single.h; not bit-exact to the double rounding the
    # generator wrote; CMake BLUR_FMADDS_SINGLE=OFF gives it back). Before the copies, so they take the same form.
    run(tool('fma_single.py'), '--gen', gen)
    # renut-ultimate (2026-11-16, port 144; tw2de_nx's fnmadd / fnmsub fix): the negated fused multiply-adds as
    # REX_FNEG_FUSED(...), the plain negation by default (the same code); CMake TW2DE_FNMADD_SIGN=ON (off by default)
    # gives the PowerPC's sign of zero and NaN (sdk/include/rex/ppc/fpu_fma.h). After fma_single.py, before the copies.
    run(tool('fnm_sign.py'), '--gen', gen)
    # sasrt-nx (2026-11-12, its docs/cpu-moonshots-1112.md idea 2; port 121): a vector load of a stack slot that four
    # 4-byte stores of the same block just wrote takes the stored values (the stores stay; REX_STACK_FWD4 checks the
    # address, REX_STACK_FWD_VERIFY compares with the real load in the hooked builds). Before the copies, so they take
    # the same form. REX_STACK_FWD_OFF (compile time): the original loads.
    run(tool('stack_forward.py'), '--gen', gen)
    # blur-nx (2026-11-13, M-23 of docs/cpu-moonshots-50-1113.md): float_loads.py again, now with the any-mode pairs
    # (CG-13: the helpers are the plain conversions, so their loads and stores go through the vector unit too), and the
    # plain single loads the steps above made after its first run (single_floats.py turns proven any-mode loads into
    # plain ones: 10,239 sites were left in the integer form). After ro_constants_cfg.py and dead_ctx_defs.py, before
    # the copies (so they take the same forms) and store_cards.py (which marks the new stores' cards). renut-ultimate
    # (2026-11-13): after port 121's stack_forward.py, which then sees the stores as port 121 measured them.
    run(tool('float_loads.py'), '--gen', gen, '--any-mode', '--doubles')  # #231: lfd too (2026-11-17)
    # blur-nx (2026-11-13, docs/cpu-near-exact-50-1113.md, the user's choice of near-exact ideas): the forms a near-exact
    # switch changes as helpers whose default is the generator's expression (#42: vmaxfp / vminfp as rex::ppc::VMaxFp /
    # VMinFp, sdk/include/rex/ppc/diet_ops.h). Before the copies, so they take the same forms.
    run(tool('near_exact.py'), '--gen', gen)
    if not scratch:
        # sasrt-nx (2026-11-12, its calling-convention stages 1-1d; port 121): whole-program liveness with the vector
        # registers, right before the copies are made from this same text (localize_native.py checks the stamp):
        # what a real call need not hand over, the vector LiveOut, the scratch vectors dead after an indirect call.
        run(tool('reg_liveness.py'), '--gen', gen, '--image', IMAGE)
        # The localized native copies (app/generated/hot, from sa2-nx): made from the text as it stands after every
        # step above, so each copy matches the function the build compiles. The list is app/src/cpu/hot/localized.txt;
        # direct_calls.py (above) has already sent the callers of those functions through their hooks.
        # 2026-10-22: and the bulk list (app/src/cpu/hot/localized_bulk.txt, from the console profiles: tools/switch_hot_bulk.py).
        run(tool('localize_native.py'), '--list', '--bulk')
        # cpu_aggressive B: the XAudio limiter's copy computes log10 only where the dB value is read (the recompiled
        # original stays as it is: the hook's guard compares the two). Switch back: BANJO_HOT_LAZY_LOG10=OFF
        # (-DBANJO_LAZY_LOG10_ON=0) or the key banjo_cpu_native_sub_82B6A678.
        run(tool('limiter_lazy_log10.py'), '--hot', os.path.join(gen, 'hot'))
    # cpu_aggressive C: the script interpreters' jump-table dispatch threaded into each handler (computed gotos, GCC
    # cross-jumping off for these functions only). Switch back: REX_SWITCH_THREAD_OFF / REX_SWITCH_GOTO_OFF.
    run(tool('switch_goto.py'), '--gen', gen, '--thread', '--only',
        'sub_8299D480,sub_829A75E8,sub_8299E2E8,sub_82C3D4E8,sub_829AA068')
    # cpu_aggressive D: proven link-register write-backs (the value ctx.lr already holds) compile to nothing on the
    # Switch; REX_LR_WRITE_SAME_CHECK (PC verify) tests every one. Before r1_local.py (its patterns name ctx.r1).
    proofs = os.path.join(ROOT, 'out', 'cpu_aggressive_proofs')
    run(tool('lr_preserve.py'), '--gen', gen, '--out', proofs, '--assume-indirect', '--rewrite',
        '--only-file', os.path.join(APP, 'config', 'lr_write_same_choice.json'))
    # cpu_aggressive E: r1 as a local mirror in the functions where it compiles cheaper (app/config/r1_local_choice.json),
    # on the whole-program proof that callees return with r1 unchanged. REX_R1_CHECK (PC verify) tests the assumed
    # calls, REX_R1_STRICT reloads after them, REX_R1_LOCAL_OFF is the original code.
    run(tool('r1_preserve.py'), '--gen', gen, '--out', os.path.join(proofs, 'strict'))
    # blur-nx (2026-11-13, near-exact #9): with BLUR_NX_R1_ASSUME=1 in the environment, the assumed proof also takes a
    # function as preserving unless its own body fails (r1_preserve.py --near-exact); the strict one stays a proof, so
    # REX_R1_CHECK (PC) tests every call the assumption covers.
    run(tool('r1_preserve.py'), '--gen', gen, '--out', os.path.join(proofs, 'assumed'), '--indirect-preserve',
        *(['--near-exact'] if os.environ.get('BLUR_NX_R1_ASSUME') == '1' else []))
    run(tool('r1_local.py'), '--gen', gen, '--proof-strict', os.path.join(proofs, 'strict', 'r1_preserve.json'),
        '--proof-assumed', os.path.join(proofs, 'assumed', 'r1_preserve.json'),
        '--only-file', os.path.join(APP, 'config', 'r1_local_choice.json'))
    # Package 27's codegen round (docs/codegen-enhancements.md), last, on the final text: VMX vsel as one NEON bit select
    # (REX_VSEL_OFF), the console profile's functions declared hot so the linker gathers them (REX_HOT_LAYOUT_OFF), and
    # inline copies of the tiny leaf functions each file calls in another (REX_INLINE_LEAVES_OFF), the last of all.
    run(tool('vsel_neon.py'), '--gen', gen)
    run(tool('hot_layout.py'), '--gen', gen, '--list', os.path.join(APP, 'config', 'hot_functions.txt'))
    run(tool('inline_leaves.py'), '--gen', gen, '--hot', os.path.join(APP, 'config', 'hot_edges.txt'))
    # banjo-nx (2026-10-19, CPU proposal 1): the store cards, after everything else (the inline copies included): every
    # store outside the stack marks its page for Renut's cached vertex and index copies (REX_STORE_CARDS_OFF).
    run(tool('store_cards.py'), '--gen', gen)
    # blur-nx (2026-11-11, CPU batch 2, docs/vector-store-ideas.md VS-14): in the copies, one card check per group of
    # card stores to the same base in a straight stretch (two at its end: the lowest and highest offset's page).
    run(tool('card_groups.py'), '--gen', gen)
    # blur-nx (2026-11-11, CPU batch 2, docs/vector-ideas-2.md VX-15 / VX-14): in the copies' f_ bodies only (the o_
    # bodies stay the guard's reference), two or four broadcast dot products merged by vmrghw / vmrglw as one
    # pairwise-add tree (sdk/include/rex/ppc/vec_batch2.h).
    run(tool('vec_dp_merge.py'), '--gen', gen)
    # blur-nx (2026-11-11, CPU batch 2, docs/vector-ideas-2.md VX-29 / VX-02): in the copies' f_ bodies, vupkd3d128 of a
    # zero register as 1.0 and vcuxwfp128 / vcsxwfp128 of a known splat as their constants.
    run(tool('vec_const_fold.py'), '--gen', gen)
    # blur-nx (2026-11-11, CPU batch 1, docs/vector-store-ideas.md VS-05): a 128-byte dcbzl as two DC ZVA (no read of
    # the line it zeroes). After store_cards.py, which finds the dcbz by its memset to put the card in front.
    run(tool('vec_dcbz_zva.py'), '--gen', gen)
    # blur-nx (2026-11-16, the 15 ms batch, CG-26): the files that write the guest FPSCR keep -ftrapping-math when the
    # rest gets -fno-trapping-math (a port's CMake option: Blur's BLUR_NO_TRAPPING_MATH); last, on the final text.
    run(tool('fp_env_files.py'), '--gen', gen)
    if scratch:
        print(f'post_codegen: optimised codegen steps done on {gen}')
        return
    config = open(os.path.join(APP, 'renut_config.toml'), encoding='utf-8').read()
    extras = [name for key, name in (('non_volatile_as_local', 'non-volatile locals'), ('non_argument_as_local', 'non-argument locals'),
                                     ('fp_mode_lookahead', 'fp mode lookahead'), ('div_inline', 'CPU rounds 1-3'))
              if re.search(r'^\s*' + key + r'\s*=\s*true', config, re.M)]
    # Written only when it changes (from sa2-nx): the file is a CMake configure dependency (app/CMakeLists.txt), and
    # rewriting it made every build re-run CMake, rebuild the generator and run the codegen again.
    text = 'optimised codegen' + (f' ({", ".join(extras)})' if extras else '')
    path = os.path.join(GEN, 'codegen_flavor.txt')
    try:
        old_text = open(path, encoding='utf-8').read()
    except OSError:
        old_text = None
    if old_text != text:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    keep_unchanged_times(GEN)
    print('post_codegen: optimised codegen steps done')


def keep_unchanged_times(gen):
    """banjo-nx (2026-10-16): a CMake re-run (any new file under app/src) runs the code generator again, which writes
    every generated file anew; the steps above then give most of them exactly the text they had. A file whose text is
    the one of the last run gets its old modification time back, so ninja does not compile it again (a full rebuild of
    the 213 files is about 20 minutes on the PC). The times and hashes of the last run: app/generated/codegen.times.json."""
    import hashlib
    import json
    record = os.path.join(gen, 'codegen.times.json')
    try:
        old = json.load(open(record, encoding='utf-8'))
    except (OSError, ValueError):
        old = {}
    new = {}
    kept = 0
    # The inputs of the generator's build step besides the manifest (codegen.d, and the generator itself).
    inputs = [os.path.join(os.path.dirname(gen), 'renut_config.toml'), os.path.join(os.path.dirname(gen), 'assets',
              'default.xex'), os.path.join(os.path.dirname(gen), 'config', 'renut_funcs.toml'),
              os.path.join(os.path.dirname(gen), 'config', 'renut_hooks.toml'),
              os.path.join(os.environ.get('REXSDK_DIR', os.path.join(ROOT, 'sdk')), 'out', 'win-amd64', 'rexglue.exe')]
    floor = max([os.stat(p).st_mtime_ns for p in inputs if os.path.isfile(p)] or [0])
    paths = []
    for folder in (gen, os.path.join(gen, 'hot')):
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if name.endswith(('.cpp', '.h', '.inc')):
                paths.append(os.path.join(folder, name))
    for path in paths:
        if os.path.isfile(path):
            rel = os.path.relpath(path, gen).replace(os.sep, '/')
            digest = hashlib.blake2b(open(path, 'rb').read(), digest_size=16).hexdigest()
            entry = old.get(rel)
            if entry and entry[0] == digest:
                # Never older than an input of the generator's step (a newer generator binary, say): ninja would
                # then run the step again on every build.
                when = max(entry[1], floor)
                os.utime(path, ns=(when, when))
                kept += 1
                new[rel] = [digest, when]
            else:
                new[rel] = [digest, os.stat(path).st_mtime_ns]
    # The generator also writes its manifest again on every run (the sdk_version stamp, the same text). It is an input
    # of its own build step (codegen.d): a manifest newer than the outputs kept above made every build run the
    # generator again. It gets a time a second before the oldest output (it was read before any of them was written).
    manifest = os.path.join(os.path.dirname(gen), 'renut_manifest.toml')
    if new and os.path.isfile(manifest):
        oldest = min(os.stat(os.path.join(gen, rel)).st_mtime_ns for rel in new)
        if os.stat(manifest).st_mtime_ns > oldest:
            os.utime(manifest, ns=(oldest, oldest))  # (an input as old as an output is not newer: ninja keeps it)
    with open(record, 'w', encoding='utf-8') as f:
        json.dump(new, f)
    print(f'post_codegen: {kept} of {len(new)} generated files unchanged since the last run (their old times kept)')


if __name__ == '__main__':
    main()
