#!/usr/bin/env python3
"""renut-ultimate port 83 (2026-11-11): sasrt-nx's tool, from its handoff profile_tools (it imports this repository's
tools/switch_hot_functions.py, which has the base's object-folder names and map path). A console profile (logs/rex/rex_profile_NNN.log) broken down by thread and by kind of code, for a window of its 10 s
reports (sasrt-nx, 2026-11-08; the method of katamari-nx's docs/optimising-functions.md "Measuring").

Per thread: its CPU share (of one core) averaged over the window, its pinning, its busy samples (not waiting in the
kernel) and how they split between
  hand      a hand-written native (app/src/hot/n_*.h, and the C runtime / Direct3D natives),
  machine   a localized copy (app/generated/hot/n_*.h, listed in app/src/hot/localized.txt),
  generated a guest function still running as plain generated code (module "default"): not optimised,
  renderer  Renut's renderer and its back end (host code),
  runtime   the SDK, libnx, the C/C++ library and everything else that is host code.
Then each thread's hottest functions with their kind, and the hottest guest functions of all threads.

  python tools/profile_breakdown.py PROFILE --map MAP [--from-report N] [--to-report M] [--top 15] [--min-cpu 1]
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switch_hot_functions as shf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = re.compile(r'^==== [\d.]+ s \| mode: (\S+) \| game ([\d.]+) fps \| CPU total (\d+)%')
THREAD = re.compile(r'^-- thread (\d+) "(.*?)": CPU ([\d.]+)% \| priority (0x[0-9A-Fa-f]+), preferred core (-?\d+), mask (0x[0-9a-f]+)')
SAMPLES = re.compile(r'^\s+(\d+) samples, (\d+)% waiting in the kernel')
PC = re.compile(r'^\s+[\d.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)')
NATIVE = re.compile(r'n_([0-9A-F]{8})')


def lists():
    machine, hand = set(), set()
    for p in glob.glob(os.path.join(ROOT, 'app', 'generated', 'hot', 'n_*.h')):
        m = NATIVE.search(os.path.basename(p))
        if m:
            machine.add(m.group(1))
    # sasrt-nx's own hand natives live in app/src/hot (none in the base, whose hand natives are listed in hand.txt below).
    for p in glob.glob(os.path.join(ROOT, 'app', 'src', 'hot', 'n_*.h')):
        m = NATIVE.search(os.path.basename(p))
        if m:
            hand.add(m.group(1))
    # 2026-11-08: katamari-nx's hand-written natives (app/src/cpu/hot/hand.txt).
    hand_txt = os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'hand.txt')
    if os.path.isfile(hand_txt):
        for line in open(hand_txt, encoding='utf-8'):
            m = re.match(r'\s*([0-9A-Fa-f]{8})\b', line)
            if m:
                hand.add(m.group(1).upper())
    return machine - hand, hand


RENDERER_OBJ = re.compile(r'(renut|draw|queued|render_thread|command_buffers|textures|surfaces|pipeline|deko3d|plume|'
                          r'shader|upload_ring|video|present|banjo_renut|sasrt_renut|vertex_format|format|callbacks|'
                          r'guest_memory|resource|state\.cpp|device\.cpp|gpu)', re.I)


def kind(name, obj, machine, hand):
    if obj == 'default':
        return 'generated'
    m = NATIVE.search(name)
    if m and m.group(1) in hand:
        return 'hand'
    if m and m.group(1) in machine:
        return 'machine'
    s = re.search(r'sub_([0-9A-F]{8})', name)
    if obj == 'app' and s:
        return 'hand' if s.group(1) in hand else ('machine' if s.group(1) in machine else 'hand')
    # renut-ultimate (port 83): the base's namespaces are banjo::hot (the copies' hooks) and banjo::hand (the hand natives).
    if obj == 'app' and ('banjo3hot' in name or 'banjo4hand' in name or 'hot::' in name):
        return 'hand'
    if obj == 'renderer' or (obj.endswith('.obj') and RENDERER_OBJ.search(obj)):
        return 'renderer'
    return 'runtime'


def main():
    a = argparse.ArgumentParser()
    a.add_argument('profile')
    a.add_argument('--map', required=True)
    a.add_argument('--from-report', type=int, default=0)
    a.add_argument('--to-report', type=int, default=10 ** 9)
    a.add_argument('--top', type=int, default=12)
    a.add_argument('--min-cpu', type=float, default=1.0)
    args = a.parse_args()
    sections = shf.load_map(args.map)
    starts = [s[0] for s in sections]
    machine, hand = lists()
    image_base = None
    reports = []
    cur = None
    thread = None
    for line in open(args.profile, encoding='utf-8', errors='replace'):
        if line.startswith('image 0x') and image_base is None:
            image_base = int(line.split()[1].rstrip(','), 16)
        m = REPORT.match(line)
        if m:
            cur = {'mode': m.group(1), 'fps': float(m.group(2)), 'cpu': int(m.group(3)), 'threads': {}}
            reports.append(cur)
            thread = None
            continue
        m = THREAD.match(line)
        if m and cur is not None:
            thread = {'name': m.group(2), 'cpu': float(m.group(3)), 'prio': m.group(4), 'core': int(m.group(5)),
                      'mask': m.group(6), 'samples': 0, 'waiting': 0, 'pcs': collections.Counter()}
            cur['threads'][m.group(1)] = thread
            continue
        m = SAMPLES.match(line)
        if m and thread is not None:
            thread['samples'] = int(m.group(1))
            thread['waiting'] = int(m.group(2))
            continue
        m = PC.match(line)
        if m and thread is not None:
            thread['pcs'][int(m.group(1), 16)] += int(m.group(2))
    window = [r for i, r in enumerate(reports) if args.from_report <= i <= args.to_report]
    n = len(window)
    print('window: reports %d-%d of %d (%d x 10 s); game %.1f fps on average (%.1f-%.1f); CPU total %.0f%% of 300%% '
          '(3 cores for the process)' % (args.from_report, min(args.to_report, len(reports) - 1), len(reports), n,
                                        sum(r['fps'] for r in window) / n, min(r['fps'] for r in window),
                                        max(r['fps'] for r in window), sum(r['cpu'] for r in window) / n))

    def lookup(off):
        i = shf.bisect.bisect_right(starts, off) - 1
        if i >= 0 and sections[i][0] <= off < sections[i][1]:
            return sections[i][2].replace('__imp__', ''), shf.module_of(sections[i][3])
        return '?', '?'

    agg = {}
    for r in window:
        for tid, t in r['threads'].items():
            key = (tid, t['name'])
            g = agg.setdefault(key, {'cpu': 0.0, 'samples': 0, 'busy': 0, 'prio': t['prio'], 'core': t['core'],
                                     'mask': t['mask'], 'pcs': collections.Counter()})
            g['cpu'] += t['cpu'] / n
            g['samples'] += t['samples']
            g['busy'] += sum(t['pcs'].values())
            g['pcs'].update(t['pcs'])
    totals = collections.Counter()
    by_func = collections.Counter()
    func_kind = {}
    func_threads = collections.defaultdict(collections.Counter)
    rows = []
    for (tid, name), g in sorted(agg.items(), key=lambda kv: -kv[1]['cpu']):
        kinds = collections.Counter()
        funcs = collections.Counter()
        for off, c in g['pcs'].items():
            fname, obj = lookup(off)
            k = kind(fname, obj, machine, hand)
            kinds[k] += c
            funcs[(fname, obj, k)] += c
        rows.append((tid, name, g, kinds, funcs))
        totals.update(kinds)
        for (fname, obj, k), c in funcs.items():
            by_func[(fname, obj, k)] += c
            func_threads[(fname, obj, k)][name] += c
    print()
    print('%-22s %6s  %-14s %7s | %s' % ('thread', 'CPU', 'pinning', 'busy', 'hand / machine / generated / renderer / runtime'))
    for tid, name, g, kinds, funcs in rows:
        if g['cpu'] < args.min_cpu:
            continue
        b = sum(kinds.values()) or 1
        pin = ('core %d' % g['core']) if g['core'] >= 0 else ('mask %s' % g['mask'])
        print('%-22s %5.1f%%  %-14s %7d | %4.1f / %4.1f / %4.1f / %4.1f / %4.1f %%' % (
            name[:22], g['cpu'], pin + ' p' + g['prio'][2:], b, 100 * kinds['hand'] / b, 100 * kinds['machine'] / b,
            100 * kinds['generated'] / b, 100 * kinds['renderer'] / b, 100 * kinds['runtime'] / b))
    t = sum(totals.values()) or 1
    print('%-22s %6s  %-14s %7d | %4.1f / %4.1f / %4.1f / %4.1f / %4.1f %%' % (
        'ALL THREADS', '', '', t, 100 * totals['hand'] / t, 100 * totals['machine'] / t, 100 * totals['generated'] / t,
        100 * totals['renderer'] / t, 100 * totals['runtime'] / t))
    for tid, name, g, kinds, funcs in rows:
        if g['cpu'] < max(args.min_cpu, 2.0):
            continue
        b = sum(kinds.values()) or 1
        print()
        print('== %s (CPU %.1f%%, %d busy samples): top %d' % (name, g['cpu'], b, args.top))
        for (fname, obj, k), c in funcs.most_common(args.top):
            print('   %5.1f%%  %-9s %-22s %s' % (100 * c / b, k, obj[:22], fname[:90]))
    print()
    print('== guest functions, all threads: top 30 by samples (share of all busy samples)')
    guest = [(c, f) for f, c in by_func.items() if f[2] in ('hand', 'machine', 'generated')]
    for c, (fname, obj, k) in sorted(guest, reverse=True)[:30]:
        where = ', '.join('%s %d' % (tn[:16], tc) for tn, tc in func_threads[(fname, obj, k)].most_common(2))
        print('   %5.2f%%  %-9s %-60s %s' % (100 * c / t, k, fname[:60], where))


if __name__ == '__main__':
    main()
