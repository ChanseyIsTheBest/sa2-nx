#!/usr/bin/env python3
"""sa2-nx on the ultimate base: the settings a build runs with, against sa2-nx 1.0.6's.

Every key a tree defines (REXCVAR_DEFINE_* in app/src and sdk/, its default as written), then the compiled-in toml and
the player's toml over it, for two trees: 1.0.6 (a git revision of sa2-nx) and this one. Prints, for every key the two
share by name or by sa2-nx's renaming (sa2_renut_X -> banjo_renut_X and the table below), the keys whose values
differ. A key's default is taken as written in its REXCVAR_DEFINE (a literal; an expression is printed as it is), so a
default computed by code (per platform, from another key) needs a look by hand: the report marks those.

  python tools/sa2_effective_keys.py [--old-repo <sa2-nx>] [--old-rev baf523b] [--all]
"""
import argparse
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# sa2-nx 1.0.6 key -> the base's key for the same thing (docs/sa2-ultimate-notes.md, "Settings").
RENAMES = {
    'sa2_renderer': 'banjo_renderer',
    'sa2_renut_msaa': 'msaa_level',
    'sa2_renut_double_buffer': 'pacman_renut_double_buffer',
    'sa2_renut_queue_drain': 'pacman_renut_queue_drain',
    'sa2_renut_lockfree_queue': 'dishonored_renut_lockfree_queue',
    'sa2_renut_colour_handover': 'banjo_deko3d_color_handover',
    'sa2_renut_gamma_ramp': 'banjo_renut_game_gamma',
    'sa2_renut_core_layout': 'banjo_renut_core_layout',
    'sa2_render_height_handheld': 'banjo_renut_render_height_handheld',
    'sa2_render_height_docked': 'banjo_renut_render_height_docked',
    'sa2_cpu_enhancements': 'banjo_cpu_enhancements',
    'sa2_cpu_verify': 'banjo_cpu_verify',
    'sa2_hot_guard_calls': 'banjo_hot_guard_calls',
    'sa2_hot_guard_period': 'banjo_hot_guard_period',
}

DEFINE = re.compile(r'REXCVAR_DEFINE_(BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|FLOAT|STRING)\(\s*([a-z0-9_]+)\s*,\s*'
                    r'((?:"(?:[^"\\]|\\.)*")|[^,]+?)\s*,', re.S)


def git_files(repo, rev, folders):
    out = subprocess.run(['git', '-C', repo, 'ls-tree', '-r', '--name-only', rev, '--'] + folders,
                         capture_output=True, text=True, check=True).stdout.split('\n')
    return [p for p in out if p.endswith(('.cpp', '.h', '.cc', '.c'))]


def git_text(repo, rev, path):
    r = subprocess.run(['git', '-C', repo, 'show', f'{rev}:{path}'], capture_output=True)
    return r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else None


def defaults_from(texts):
    keys = {}
    for path, text in texts:
        for m in DEFINE.finditer(text):
            kind, name, value = m.groups()
            keys.setdefault(name, (kind, norm(value.strip()), path))
    return keys


def norm(v):
    v = v.strip()
    if v.startswith('"') and v.endswith('"'):
        return v[1:-1]
    low = v.lower()
    if low in ('true', 'false'):
        return low
    v = re.sub(r'(?<=\d)[uUlLfF]+$', '', v)
    try:
        f = float(v)
        return str(int(f)) if f == int(f) else str(f)
    except ValueError:
        return v  # an expression: printed as it is


def toml_root(text):
    """Root keys of a toml (before its first table), as normalised strings."""
    out = {}
    for line in text.split('\n'):
        s = line.strip()
        if s.startswith('['):
            break
        if not s or s.startswith('#') or '=' not in s:
            continue
        k, v = s.split('=', 1)
        v = v.split(' #')[0].strip()
        out[k.strip()] = norm(v)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old-repo', default=os.path.join(os.path.dirname(ROOT), 'sa2-nx'))
    ap.add_argument('--old-rev', default='baf523b')
    ap.add_argument('--all', action='store_true', help='also the keys only one tree has')
    a = ap.parse_args()

    old_files = git_files(a.old_repo, a.old_rev, ['app/src', 'sdk/src', 'sdk/include'])
    old = defaults_from((p, git_text(a.old_repo, a.old_rev, p) or '') for p in old_files)
    for toml in ('app/sa2_switch_defaults.toml', 'app/sa2_switch.toml'):
        for k, v in toml_root(git_text(a.old_repo, a.old_rev, toml) or '').items():
            if k in old:
                old[k] = (old[k][0], v, toml)
            else:
                old[k] = ('?', v, toml + ' (no define)')

    new_files = []
    for top in ('app/src', 'sdk/src', 'sdk/include'):
        for dirpath, _, names in os.walk(os.path.join(ROOT, top)):
            for n in names:
                if n.endswith(('.cpp', '.h', '.cc', '.c')):
                    new_files.append(os.path.join(dirpath, n))
    new = defaults_from((os.path.relpath(p, ROOT), open(p, encoding='utf-8', errors='replace').read())
                        for p in new_files)
    for toml in ('app/sa2_switch_defaults.toml', 'app/sa2_switch.toml'):
        path = os.path.join(ROOT, toml)
        if os.path.exists(path):
            for k, v in toml_root(open(path, encoding='utf-8').read()).items():
                if k in new:
                    new[k] = (new[k][0], v, toml)
                else:
                    new[k] = ('?', v, toml + ' (no define)')

    pairs = []
    for k, (kind, v, src) in old.items():
        nk = RENAMES.get(k)
        if nk is None and k in new:
            nk = k
        if nk is None and k.startswith('sa2_renut_') and 'banjo_renut_' + k[10:] in new:
            nk = 'banjo_renut_' + k[10:]
        if nk is None and k.startswith('sa2_') and 'banjo_' + k[4:] in new:
            nk = 'banjo_' + k[4:]
        if nk is None:
            if a.all:
                print(f'only 1.0.6   {k} = {v}  ({src})')
            continue
        nv = new.get(nk, ('', '(not defined)', ''))
        pairs.append((k, v, src, nk, nv[1], nv[2]))
    diff = [p for p in pairs if p[1] != p[4]]
    print(f'{len(old)} keys in 1.0.6, {len(new)} here, {len(pairs)} paired, {len(diff)} differ:')
    for k, v, src, nk, nv, nsrc in sorted(diff, key=lambda p: p[0]):
        name = k if k == nk else f'{k} -> {nk}'
        print(f'  {name}: 1.0.6 {v} ({src}) | here {nv} ({nsrc})')


if __name__ == '__main__':
    main()
