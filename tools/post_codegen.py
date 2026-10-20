#!/usr/bin/env python3
"""The post-codegen steps of the codegen flavour, in one place (docs/enhancements.md, "Build-time").

Run after EVERY codegen, by tools/codegen.ps1 and by the build itself: app/CMakeLists.txt appends this script to the
generated sa2_codegen command, because the PC build re-runs the generator on its own whenever an input changes (the
manifest, a toml, the xex, the generator binary) and that would otherwise drop the steps silently.

  - the flavour is read from app/codegen_flavor.toml (written by tools/codegen.ps1 -Flavor): "optimised" when it sets
    any codegen key, else "classic";
  - optimised: tools/pch_no_volatile.py + tools/loop_barriers.py (required together) and tools/direct_calls.py on both
    generated trees; then sasrt-nx's steps of 2026-10-12, each a rename of calls into macros the generated pch
    defines: tools/guest_cas.py (compare-and-swap without the legacy builtin's trailing `dmb ish` on AArch64),
    tools/flush_mode_flow.py (the flush-mode checks the control flow and the callees' summaries fix: nothing on the
    Switch, still tested in the PC build), tools/vec_byteswap.py (the 16-byte reversal of guest vector loads and
    stores without a table lookup on AArch64) and tools/pch_neon_shuffles.py (constant word shuffles as one
    permutation on AArch64);
  - optimised, the second CPU round of 2026-10-12 (each step has its own compile-time switch back, see the tool):
      tools/indirect_calls.py   every `bctrl` tagged with its site (PC census), and the sites of
                                app/indirect_promotions.txt as a guarded direct call. Before direct_calls.py.
      tools/ro_constants.py     loads from the image's read-only data as constants. Before float_loads.py.
      tools/float_loads.py      a single-precision load straight into a float register
      tools/frame_traffic.py    the link register and the non-volatile saves of the function prologues
      tools/leaf_locals.py      the ctx registers of leaf functions with loops as locals. Last: it renames registers.
      tools/pch_cold_traps.py   the pch's trap helper (before every division) out of line and cold
  - optimised, the third CPU round of 2026-10-12:
      tools/loop_locals.py      the ctx registers a call-free loop uses as locals for the length of the loop (the
                                barrier stays). After loop_barriers.py, before the localized copies and
                                leaf_locals.py (which takes the leaf functions).
    ro_constants.py and the entry modes of flush_mode_flow.py need the generator's image dump (out/default_image.bin,
    written by tools/codegen.ps1 through REXGLUE_DUMP_IMAGE); the steps whose PC form calls into the app
    (indirect_calls, ro_constants) run on the game's tree only, not on the stage module (a DLL of its own on the PC);
  - optimised, banjo-nx's codegen round (ported 2026-10-19; docs/enhancements.md "banjo-nx's codegen round"), on the
    game's tree, in banjo-nx's order after the loop and leaf locals:
      tools/vperm_one_source.py, tools/fp_sign_ops.py, tools/leaf_dead_stores.py, tools/single_floats.py,
      tools/inline_memcalls.py, tools/vec_unaligned_stores.py, tools/ro_constants_cfg.py, tools/dead_ctx_defs.py,
      tools/flush_hoist.py; after the localized copies tools/lr_preserve.py --rewrite, tools/vsel_neon.py and
      tools/inline_leaves.py (last). The late rewrites are taken back out first on a tree processed before, so every
      analysis reads the text it was written for. ro_constants.py and ro_constants_cfg.py get --xex (import records).
  - both: the localized native copies (tools/localize_native.py --list app/src/cpu/hot/localized.txt) and
    app/generated/codegen_flavor.txt (the build's name in the first "[enh]" log line).

Every step is idempotent and the whole sequence is too: a second run of this script on a processed tree changes no
file (checked on a copy: tools/scratch_ab.py copy, then this script twice with --gen out/gen_scratch and a hash of the
tree after each run; the profile of a PGO build fits only the exact
text it was made from).

  python tools/post_codegen.py [--gen <generated folder>]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
IMAGE = os.path.join(ROOT, 'out', 'default_image.bin')
XEX = os.path.join(ROOT, 'assets', 'game_root', 'default.xex')
PROMOTIONS = os.path.join(APP, 'indirect_promotions.txt')


def run(*args):
    result = subprocess.run([sys.executable, *args], cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f'post_codegen: {" ".join(os.path.basename(a) for a in args[:1])} failed ({result.returncode})')


def tool(name):
    return os.path.join(ROOT, 'tools', name)


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    scratch = gen != GEN  # a copy for measurements (tools/scratch_ab.py): no localized copies, no flavour file
    flavor_toml = open(os.path.join(APP, 'codegen_flavor.toml'), encoding='utf-8').read()
    optimised = re.search(r'^\s*[a-z_]+\s*=\s*true', flavor_toml, re.M) is not None
    flavor = 'optimised' if optimised else 'classic'
    trees = [t for t in ('default', 'data_dll') if os.path.isdir(os.path.join(gen, t))]
    if optimised:
        if not os.path.isfile(IMAGE):
            sys.exit('post_codegen: out/default_image.bin is missing (the image dump of the code generator): run '
                     'tools\\codegen.ps1 -Flavor optimised once, it writes it (REXGLUE_DUMP_IMAGE)')
        xex = os.path.join(ROOT, 'assets', 'game_root', 'default.xex')
        if os.path.isfile(xex) and os.path.getmtime(xex) > os.path.getmtime(IMAGE):
            sys.exit('post_codegen: out/default_image.bin is older than assets/game_root/default.xex: run '
                     'tools\\codegen.ps1 -Flavor optimised (it writes the dump again)')
        # banjo-nx's late rewrites out first (they change statements the analyses parse); nothing on a fresh codegen.
        game = os.path.join(gen, 'default')
        proofs = os.path.join(ROOT, 'out', 'codegen_proofs')
        # sa2-nx (2026-10-20): the store cards were the last step: out first, on both trees and the localized copies.
        for tree in trees:
            run(tool('store_cards.py'), '--gen', os.path.join(gen, tree), '--hot', os.path.join(gen, 'hot'), '--restore')
        run(tool('inline_leaves.py'), '--gen', game, '--restore')
        run(tool('vsel_neon.py'), '--gen', game, '--restore')
        run(tool('lr_preserve.py'), '--gen', game, '--out', proofs, '--restore')
        run(tool('flush_hoist.py'), '--gen', game, '--restore')
        # And the round's other rewrites (peggle2-nx's round_restore.py): the base steps decided differently on their
        # forms on a second run (loop barriers, flush proofs, read-only constants), and a PGO profile fits only the
        # exact text it was made from. Two runs now give identical files.
        run(tool('round_restore.py'), '--gen', game)
        run(tool('pch_no_volatile.py'), *[os.path.join(gen, t, 'sa2_pch.h') for t in trees])
        os.environ.setdefault('REXSDK_DIR', os.path.join(ROOT, 'sdk'))
        # Second round: the tagged and promoted indirect calls. Before direct_calls.py (a promoted call is a direct
        # call like any other: __imp__ where its target has no hook) and BEFORE loop_barriers.py: a loop needs no
        # barrier when every way round it passes an indirect call, and a promoted call is no longer one (the
        # compiler can inline its target and see that it leaves the polled memory alone).
        run(tool('indirect_calls.py'), '--gen', os.path.join(gen, 'default'), '--promote', PROMOTIONS)
        # REQUIRED with pch_no_volatile.py: without barriers GCC turns guest polling loops into infinite loops.
        run(tool('loop_barriers.py'), *[os.path.join(gen, t) for t in trees])
        run(tool('pch_cold_traps.py'), *[os.path.join(gen, t, 'sa2_pch.h') for t in trees])
        for tree in trees:
            # Undo and redo in one pass: a function hooked since the last run (a new native) must be called through
            # its hook again. --redo writes only the files that change, so the build does not recompile the others.
            run(tool('direct_calls.py'), '--redo', '--gen', os.path.join(gen, tree))
        for tree in trees:
            # sasrt-nx's steps (2026-10-12). After direct_calls.py: the flush-mode proofs depend on which callees are
            # called directly, and --redo drops proofs from before a function was hooked. They only rename calls to
            # macros of the generated pch, so the localized copies below are made from the same statements.
            run(tool('guest_cas.py'), '--gen', os.path.join(gen, tree))
            # The entry modes (second round) need the image: the game's tree only.
            run(tool('flush_mode_flow.py'), '--redo', '--gen', os.path.join(gen, tree),
                *(['--image', IMAGE] if tree == 'default' else []))
            run(tool('vec_byteswap.py'), '--gen', os.path.join(gen, tree))
            run(tool('pch_neon_shuffles.py'), os.path.join(gen, tree, 'sa2_pch.h'))
        # Second round, the rest. ro_constants before float_loads (it folds the integer form of a float load).
        # --xex: loads of the XEX's import records (kernel variable addresses the loader writes) stay loads (from
        # futari-nx / peggle2-nx via banjo-nx: 86 such loads were folded in SA2's tree).
        run(tool('ro_constants.py'), '--gen', os.path.join(gen, 'default'), '--image', IMAGE, '--xex', XEX)
        for tree in trees:
            run(tool('float_loads.py'), '--gen', os.path.join(gen, tree))
            run(tool('frame_traffic.py'), '--gen', os.path.join(gen, tree))
    if optimised:
        for tree in trees:
            # Third round: the ctx registers of call-free loops as locals for the length of the loop. Before the
            # localized copies, which are made from this text and rename its ctx registers once more.
            run(tool('loop_locals.py'), '--gen', os.path.join(gen, tree))
        for tree in trees:
            # Before banjo-nx's analyses (they read its forms) and the localized copies (localize_native.py reads
            # both forms).
            run(tool('leaf_locals.py'), '--gen', os.path.join(gen, tree))
        # banjo-nx's codegen round, on the game's tree (the stage module calls into it through pointers only, which
        # the analyses take for address-taken functions).
        game = os.path.join(gen, 'default')
        run(tool('vperm_one_source.py'), '--gen', game)
        # fp_sign_ops: exact while the game never selects round-up / round-down (the "[census] FPSCR rounding mode"
        # line of sa2_diag_runtime_census counts the guest's choices).
        run(tool('fp_sign_ops.py'), '--gen', game)
        run(tool('leaf_dead_stores.py'), '--gen', game, '--image', IMAGE)
        run(tool('single_floats.py'), '--gen', game)
        run(tool('inline_memcalls.py'), '--gen', game)
        run(tool('vec_unaligned_stores.py'), '--gen', game)
        run(tool('ro_constants_cfg.py'), '--gen', game, '--image', IMAGE, '--xex', XEX)
        if os.environ.get('SA2_DEAD_DEFS_OFF'):
            run(tool('dead_ctx_defs.py'), '--gen', game, '--restore-only')
        elif not os.environ.get('SA2_DEAD_DEFS_HOLD'):  # tools/dead_defs_bisect.py has set the files: leave them
            run(tool('dead_ctx_defs.py'), '--gen', game, '--image', IMAGE, '--abi-indirect')
        run(tool('flush_hoist.py'), '--gen', game)
    if not scratch:
        run(tool('localize_native.py'), '--list', os.path.join(APP, 'src', 'cpu', 'hot', 'localized.txt'))
    if optimised:
        game = os.path.join(gen, 'default')
        proofs = os.path.join(ROOT, 'out', 'codegen_proofs')
        # Link-register write-backs proven to store the value ctx.lr already holds compile to nothing on the Switch;
        # the PC build checks every one (REX_LR_WRITE_SAME_CHECK). Strictly, without --assume-indirect: peggle2-nx's
        # PC check found 1,473 wrong write-backs with it (functions that end through __restgprlr, reached through
        # pointers or hooks).
        run(tool('lr_preserve.py'), '--gen', game, '--out', proofs, '--rewrite')
        run(tool('vsel_neon.py'), '--gen', game)
        # Last of all: inline copies of the tiny leaves each file calls in another.
        run(tool('inline_leaves.py'), '--gen', game)
        # sa2-nx (2026-10-20, banjo-nx's CPU proposal 1): the store cards, after everything else (the inline copies and
        # the localized copies included): every store outside the stack marks its page for Renut's cached copies
        # (REX_STORE_CARDS_OFF compiles them out). The localized copies (app/generated/hot) once, with the game's tree.
        for tree in trees:
            run(tool('store_cards.py'), '--gen', os.path.join(gen, tree),
                *(['--hot', os.path.join(gen, 'hot')] if tree == 'default' else ['--hot', '-']))
    if scratch:
        print(f'post_codegen: {flavor} codegen steps done on {gen}')
        return
    # Round 7: the options added since round 6 are named too, so a console log tells the builds apart.
    extras = [name for key, name in (('non_volatile_as_local', 'non-volatile locals'), ('elide_lr_stores', 'no lr stores'),
                                     ('fp_moves_modeless', 'modeless fp moves'),
                                     ('non_argument_as_local', 'non-argument locals'),
                                     ('fp_mode_lookahead', 'fp mode lookahead'),
                                     ('fcti_inline', 'second round'))
              if re.search(r'^\s*' + key + r'\s*=\s*true', flavor_toml, re.M)]
    # Written only when it changes: the file is a CMake configure dependency (app/CMakeLists.txt), and rewriting it
    # made every build re-run CMake, rebuild the generator and run the codegen again (2026-10-07).
    text = f'{flavor} codegen' + (f' ({", ".join(extras)})' if extras else '')
    path = os.path.join(GEN, 'codegen_flavor.txt')
    try:
        old_text = open(path, encoding='utf-8').read()
    except OSError:
        old_text = None
    if old_text != text:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    keep_unchanged_times(GEN)
    print(f'post_codegen: {flavor} codegen steps done')


def keep_unchanged_times(gen):
    """From banjo-nx (2026-10-16): a CMake re-run runs the code generator again, which writes every generated file
    anew; the steps above then give most of them exactly the text they had. A file whose text is the one of the last
    run gets its old modification time back, so ninja does not compile it again. The times and hashes of the last
    run: app/generated/codegen.times.json."""
    import hashlib
    import json
    record = os.path.join(gen, 'codegen.times.json')
    try:
        old = json.load(open(record, encoding='utf-8'))
    except (OSError, ValueError):
        old = {}
    new = {}
    kept = 0
    # The inputs of the generator's build step besides the manifest: never give an output a time older than these
    # (ninja would then run the step on every build).
    inputs = [os.path.join(APP, n) for n in ('codegen_flavor.toml', 'overrides.toml', 'overrides_data_dll.toml')]
    inputs += [XEX] + [os.path.join(ROOT, 'sdk', 'out', 'win-amd64', n) for n in ('rexglue.exe', 'rexgluerd.exe')]
    floor = max([os.stat(p).st_mtime_ns for p in inputs if os.path.isfile(p)] or [0])
    paths = []
    for folder in (os.path.join(gen, 'default'), os.path.join(gen, 'data_dll'), os.path.join(gen, 'hot')):
        if os.path.isdir(folder):
            paths += [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if n.endswith(('.cpp', '.h', '.inc'))]
    for path in paths:
        rel = os.path.relpath(path, gen).replace(os.sep, '/')
        digest = hashlib.blake2b(open(path, 'rb').read(), digest_size=16).hexdigest()
        entry = old.get(rel)
        if entry and entry[0] == digest:
            when = max(entry[1], floor)
            os.utime(path, ns=(when, when))
            kept += 1
            new[rel] = [digest, when]
        else:
            new[rel] = [digest, os.stat(path).st_mtime_ns]
    # The generator writes its manifest again on every run (the same text); a manifest newer than the outputs kept
    # above would make every build run the generator again. It gets the time of the oldest output.
    manifest = os.path.join(APP, 'sa2_manifest.toml')
    if new and os.path.isfile(manifest):
        oldest = min(os.stat(os.path.join(gen, rel)).st_mtime_ns for rel in new)
        if os.stat(manifest).st_mtime_ns > oldest:
            os.utime(manifest, ns=(oldest, oldest))
    with open(record, 'w', encoding='utf-8') as f:
        json.dump(new, f)
    print(f'post_codegen: {kept} of {len(new)} generated files unchanged since the last run (their old times kept)')


if __name__ == '__main__':
    main()
