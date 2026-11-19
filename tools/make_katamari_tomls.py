#!/usr/bin/env python3
"""katamari-nx (2026-10-30): writes the package's tomls from banjo-nx's shipped renut.toml (its SD package of
2026-10-29 evening), with Beautiful Katamari's settings:

  renut.toml       the shipped settings: Renut on deko3d, 720 lines handheld and 1080 docked (the user's choice; no
                   dynamic resolution, the game's own MSAA), every exact enhancement as banjo-nx ships it, the items measured
                   SLOWER ON THE CONSOLE off, diagnostics as banjo-nx's renut.toml (logs on);
  renut_play.toml  renut.toml with the render-thread diagnostics off (banjo-nx's renut_play.toml difference);
  renut_ab.toml    renut.toml with an in-run A/B list, 30 s halves, target 1 (a Vulkan-only key: does nothing on the
                   Switch) as the control.
  configs/*.toml   the picture-changing options, each one key away from renut.toml, for the user to decide on.

The NRO reads one toml: katamari.toml (KATAMARI_BUILTIN_CONFIG on, the default) or renut.toml (off). These do something
only under that name: tools/package_switch.ps1 names the package's copy.

  python tools/make_katamari_tomls.py --banjo <banjo-nx renut.toml> --out dist/switch/katamari-nx
"""
import argparse
import os
import re

HEADER = """# katamari-nx (Beautiful Katamari on the Switch, Renut's renderer on deko3d): {what}
# Made by tools/make_katamari_tomls.py from banjo-nx's shipped renut.toml (2026-10-29). Copy over katamari.toml next to
# the NRO, the only toml it reads (renut.toml with KATAMARI_BUILTIN_CONFIG off). Credits: reNut's renderer (masterspike52,
# reuse permitted), banjo-nx, sa2-nx, NetherSX2_nx (four-core layout), battd_nx (the GPU arena), bejeweled3-nx and the zuma
# ports (hot functions).
"""

# Keys set for Beautiful Katamari (the rest as banjo-nx ships them).
SHIPPED = {
    'banjo_renderer': '"renut"',
    'renut_gpu_backend': '"deko3d"',
    # The game's MSAA, no dynamic resolution. Handheld at the console's 720 lines; docked at 1080 lines, the user's
    # decision (2026-10-31: "draw at 1080p in docked and 720p in handheld"; rc4 and rc5 still shipped 720 docked,
    # fixed in rc6). configs/renut_720_docked.toml keeps the Xbox 360's 720 lines docked too.
    'banjo_renut_render_height_handheld': '720',
    'banjo_renut_render_height_docked': '1080',
    'banjo_renut_dynamic_resolution': 'false',
    'disable_msaa': 'false',
    'renut_shadow_resolution': '"1x"',
    'renut_ambient_occlusion': '"Half"',
    'renut_post_process_precision': '"Game"',
    # The game paces itself (its presentation interval); no extra cap.
    'frame_cap': '"Off"',
    'target_refreshRate': '-1',
    # banjo-nx's PM4 renderer and its keys are not used here.
    'masseffect_renderer_native': 'false',
    'gpu_plugin': '""',
    # No four-core NPDM (2026-11-01, the user: "let's just focus on 3 core performance"): empty, the NRO writes none.
    # rc5 named the user's forwarder (0100080021954000) and wrote its NPDM on the first launch; deleting
    # sdmc:/atmosphere/contents/0100080021954000/exefs/main.npdm puts that forwarder back on three cores.
    'banjo_forwarder_program_id': '""',
    # The XMA frame cache off (2026-10-31): in the rc4 console run it saved 0-8 % of the decoding and re-decoded
    # 10,000-23,000 frames per 10 s in bursts "to catch the codec up", which line up with the worst kick-to-data
    # delays (16-18 ms) while sounds stalled. Exact either way (the same samples).
    'banjo_xma_frame_cache': 'false',
    'banjo_shader_misses': '"sdmc:/switch/katamari-nx/shader_misses"',
    # Katamari's hand-written natives (docs/katamari-notes.md, "Hot functions"); the localized copies' keys
    # (banjo_cpu_native_sub_*) default to on. banjo-nx's direct dispatch (no guard) stays as it ships.
    'katamari_hand_matrix_copy': 'true',
    'katamari_hand_matrix_multiply': 'true',
    'katamari_hand_matrix_translation': 'true',
    # The sphere-test native (2026-10-31) is off until a guarded run has reached it: the emulator's scripted run
    # stays in the practice room, where the job threads' collision code never runs. renut_verify.toml has it on.
    'katamari_hand_sphere_box': 'false',
    # The user's switch (2026-11-01), off by default (their choice): the nine DLC packs that only unlock presents and
    # cousins (Accessory Sets A-D, Character Sets A-D, Special Accessory) are kept from the game; the stage packs stay.
    'katamari_dlc_unlock_packs': 'false  # true: also load the accessory / character / special accessory packs',
}
PLAY = {'banjo_renut_diag_resolve_trace': 'false', 'banjo_renut_diag_gpu_shaders': 'false',
        'banjo_renut_diag_terrain': 'false'}
# The targets that act on Beautiful Katamari's draws (direct calls, no command-buffer recordings: the census of
# docs/katamari-notes.md): the fast draw (25), pipelines by content (28), whole-buffer copies (22), the setters' touch
# compare (26), merged draws (33), mesh objects (42); 1 (a Vulkan-only key) is the do-nothing control on the Switch.
# The record-time items (45-50) have nothing to act on here. Katamari's CPU targets: 69 the hand-written matrix
# natives, 70 the localized copies of the hot functions (both back to the generated code in their off half).
# 9 targets x 60 s = 9 minutes a round: 27+ minutes.
AB = {'banjo_renut_ab_targets': '"25,28,22,26,33,42,69,70,1"', 'banjo_renut_ab_s': '30',
      'banjo_renut_dynamic_resolution': 'false'}
VERIFY = {k: 'true' for k in (
    'banjo_renut_fast_draw_verify', 'banjo_renut_resource_objects_verify', 'banjo_renut_fetch_cache_verify',
    'banjo_renut_whole_buffers_verify', 'banjo_renut_persistent_shared_verify', 'banjo_renut_chain_reuse_verify',
    'banjo_renut_snapshot_deltas_verify', 'banjo_renut_store_cards_verify', 'banjo_renut_record_extents_verify')}
# The CPU natives' guard (banjo_hot_guest.cpp): each thread's first 4000 calls of every native compared with the
# generated original, then one in 64 ("[hot] DIFFERENCE" lines; the 10-s "[hot]" report counts the checks).
VERIFY.update({'banjo_hot_guard_calls': '4000', 'banjo_hot_guard_period': '64',
               'katamari_hand_sphere_box': 'true',
               # rc10: sub_82277E70's native checks itself (no guard can repeat it): every call here.
               'katamari_hand_transform_chain_verify': 'true'})
# 2026-11-02, the user's PGO build: "turn off all logging besides crash logging". The log off (no file is made:
# sdk/src/core/logging.cpp; deko3d.log neither), the samplers, the stall watchdog and the renderer's diagnostics off
# (they would also put their own code in the profile, as banjo-nx's profile sessions turned them off). The crash
# handler's rex_crash.log and the crash hooks' rex_stderr.log (abort, terminate and Mesa messages) stay.
PGO = {'log_level': '"off"', 'switch_profile_stacks': 'false', 'banjo_cpu_profiler': 'false',
       'banjo_show_profiler': 'false', 'banjo_show_fps': 'false', 'banjo_stall_watch_seconds': '0',
       'renut_gpu_profile': '0.0', 'banjo_renut_diag_resolve_trace': 'false', 'banjo_renut_diag_gpu_shaders': 'false',
       'banjo_renut_diag_terrain': 'false', 'banjo_hot_guard_calls': '0', 'banjo_hot_guard_period': '0'}
CONFIGS = {
    'renut_verify.toml': (VERIFY, 'every Renut verify mode on (emulator / test runs: slow; the log\'s verify lines '
                                  'must count 0 differences)'),
    'renut_pgo.toml': (PGO, 'the shipped settings with only the crash logs (a PGO build\'s toml: no log, no '
                            'profilers, no diagnostics)'),
    # renut_nomsaa.toml left out (2026-10-30): disable_msaa acts through banjo-nx's mid-asm hooks at N&B's render-target
    # creation (renut_engine/hooks.cpp, not built here); in this port the key changes nothing (the emu3 run still
    # resolved 4-sample depth). It needs hooks at Katamari's own render-target creation first.
    'renut_648_864.toml': ({'banjo_renut_render_height_handheld': '648', 'banjo_renut_render_height_docked': '864'},
                           'banjo-nx\'s internal resolution: 648 lines handheld, 864 docked (not pixel-identical)'),
    'renut_720_docked.toml': ({'banjo_renut_render_height_docked': '720'},
                              '720 lines docked too, the Xbox 360\'s own (pixel-identical)'),
    'renut_dynamic.toml': ({'banjo_renut_dynamic_resolution': 'true'},
                           'banjo-nx\'s dynamic resolution, 540-720 handheld / 720-1080 docked (not pixel-identical)'),
}


def apply(text, keys):
    """Sets each key: in place where the base has it at the top level, else before the first [section] (banjo-nx's
    [input_p1] / [input_p2] stay last: a key after a table header belongs to that table)."""
    first_section = re.search(r'^\[', text, re.M)
    top_end = first_section.start() if first_section else len(text)
    for k, v in keys.items():
        pat = re.compile(r'^(\s*)' + re.escape(k) + r'\s*=.*$', re.M)
        m = pat.search(text, 0, top_end)
        if m:
            text = text[:m.start()] + f'{m.group(1)}{k} = {v}' + text[m.end():]
        else:
            text = text[:top_end].rstrip('\n') + f'\n{k} = {v}\n\n' + text[top_end:]
        first_section = re.search(r'^\[', text, re.M)
        top_end = first_section.start() if first_section else len(text)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--banjo', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    base = open(a.banjo, encoding='utf-8').read().replace('sdmc:/switch/banjo-nx/', 'sdmc:/switch/katamari-nx/')
    shipped = apply(base, SHIPPED)
    os.makedirs(os.path.join(a.out, 'configs'), exist_ok=True)
    outs = {
        'renut.toml': (shipped, 'the built-in settings with logs, profiler and diagnostics on (720 lines handheld, 1080 docked)'),
        'renut_play.toml': (apply(shipped, PLAY), 'renut.toml with the render-thread diagnostics off, for playing'),
        'renut_ab.toml': (apply(shipped, AB), 'renut.toml with an in-run A/B list, 30 s halves, target 1 as the '
                                              'control (a Vulkan-only key: nothing on the Switch)'),
    }
    for name, (cfg, what) in CONFIGS.items():
        outs[os.path.join('configs', name)] = (apply(shipped, cfg), what)
    for name, (text, what) in outs.items():
        with open(os.path.join(a.out, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(HEADER.format(what=what) + text)
        print(name)


if __name__ == '__main__':
    main()
