"""Reduces a census log (banjo_diag_d3d_census, app/src/me/banjo_d3d_census.h) to tables per scene.

   python tools/d3d/d3d_census_report.py <log> [--scene name:from-to ...] [--top N] [--devices]

--scene: seconds of the census clock (the "report at t=" value of each 10 s report); default: the route of the usual
gameplay test (A every 6 s from second 50): title 0-50, menus 50-110, intro 110-260, gameplay 260-9999.
Per function: outermost calls per Swap and PM4 words per Swap (nested calls in brackets); --devices splits by device.
"""
import collections, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
# Names: the named entry points, and the internal functions this port has read (docs/d3d-native.md).
NAMES = {
    '8222BF10': 'draw-time shader load', '8222CB90': 'flush: register words', '8222D0E8': 'flush: constant blocks',
    '8222AB88': 'flush: fetch constants', '8226AAB8': 're-send block (2203/2000/2104)', '8220E2B0': '0x2388 block',
    '82222888': 'FlushState (standalone)', '82210AB0': 'kick / next segment', '821E2940': 'more space (flush)',
    '8222D240': 'inline-data allocator', '8220F800': 'tile-extents list: new block', '8222A798': 'depth/2203 update',
    '8222BDC8': 'shader literal constants', '82656EE0': 'inline vertex shader copy', '821F7C18': 'command buffer space (n words)',
}


def names():
    out = dict(NAMES)
    try:
        toml = open(os.path.join(ROOT, 'app', 'config', 'renut_gpu_funcs.toml'), encoding='utf-8').read()
        for a, n in re.findall(r'^0x([0-9A-Fa-f]{8})\s*=\s*\{\s*name\s*=\s*"([^"]+)"', toml, re.M):
            out[a.upper()] = n
    except OSError:
        pass
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    log = args[0]
    scenes = []
    top = 40
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == '--scene':
            n, r = argv[i + 1].split(':')
            lo, hi = r.split('-')
            scenes.append((n, float(lo), float(hi)))
        if a == '--top':
            top = int(argv[i + 1])
    args = [a for a in args if ':' not in a or os.path.exists(a)]
    if not scenes:
        scenes = [('title', 0, 50), ('menus', 50, 110), ('intro', 110, 260), ('gameplay', 260, 9999)]
    split = '--devices' in sys.argv
    name = names()
    REPORT = re.compile(r'\[d3d census\] report at t=(\d+) s: (\d+) Swaps')
    FN = re.compile(r'\[d3d census\] fn ([0-9A-F]{8}) (D\S+): (\d+) calls \((\d+) nested\), (\d+) PM4 words, [\d.]+ words/call, (\d+) segments; packets:(.*)$')
    DEV = re.compile(r'\[d3d census\] device (D\d) = ([0-9A-F]{8}): calls by thread:(.*?) \| draw-time flags \(\+10940\.\.\+10944\):(.*?) \| dirty per draw call \(sums\): VS blocks (\d+), PS blocks (\d+), fetch bits (\d+), group bits (\d+)')
    PATH = re.compile(r'\[d3d census\] (D\d) (Draw\w+): (\d+) calls, (\d+) flush paths; paths.*?:(.*)$')
    swaps = collections.Counter()
    calls = collections.defaultdict(collections.Counter)
    nested = collections.defaultdict(collections.Counter)
    words = collections.defaultdict(collections.Counter)
    packets = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    flags = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    threads = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    paths = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    dirty = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    devices = {}
    scene = None
    for ln in open(log, encoding='utf-8', errors='ignore'):
        m = REPORT.search(ln)
        if m:
            t = float(m.group(1)) - 5  # the middle of the interval
            scene = next((n for n, lo, hi in scenes if lo <= t < hi), None)
            if scene:
                swaps[scene] += int(m.group(2))
            continue
        if scene is None:
            continue
        m = FN.search(ln)
        if m:
            key = (m.group(1), m.group(2)) if split else (m.group(1), '')
            calls[scene][key] += int(m.group(3))
            nested[scene][key] += int(m.group(4))
            words[scene][key] += int(m.group(5))
            for sig, n in re.findall(r'\[([^\]]+)\] x(\d+);', m.group(7)):
                packets[scene][key][sig] += int(n)
            continue
        m = DEV.search(ln)
        if m:
            devices[m.group(1)] = m.group(2)
            for th, n in re.findall(r'(T\d+) x(\d+);', m.group(3)):
                threads[scene][m.group(1)][th] += int(n)
            for f, n in re.findall(r'\[([0-9A-F]{10})\] x(\d+);', m.group(4)):
                flags[scene][m.group(1)][f] += int(n)
            for k, v in zip(('VS blocks', 'PS blocks', 'fetch bits', 'group bits'), m.groups()[4:]):
                dirty[scene][m.group(1)][k] += int(v)
            continue
        m = PATH.search(ln)
        if m:
            for bits, n in re.findall(r'([0-9A-F]{5}) x(\d+);', m.group(5)):
                paths[scene][(m.group(1), m.group(2))][bits] += int(n)
    print('devices:', ', '.join('%s = %s' % kv for kv in sorted(devices.items())))
    for sc, lo, hi in scenes:
        n = swaps[sc]
        if not n:
            continue
        print('\n=== %s (census seconds %g-%g): %d Swaps' % (sc, lo, hi, n))
        total_words = sum(words[sc].values())
        print('PM4 words per Swap written by counted outermost calls: %.0f' % (total_words / n))
        order = sorted(calls[sc], key=lambda k: -(words[sc][k] * 1000 + calls[sc][k] + nested[sc][k]))
        print('%-10s %-3s %-36s %12s %12s %12s  %s' % ('function', 'dev', 'name', 'calls/Swap', '(nested)', 'words/Swap', 'packets (per Swap)'))
        for key in order[:top]:
            a, d = key
            pk = ', '.join('%s %.1f' % (s, c / n) for s, c in packets[sc][key].most_common(5))
            print('%-10s %-3s %-36s %12.2f %12.2f %12.1f  %s' % (a, d, name.get(a, '')[:36], calls[sc][key] / n, nested[sc][key] / n, words[sc][key] / n, pk))
        for d in sorted(flags[sc]):
            print('  %s threads: %s | flags: %s | dirty per Swap: %s' % (
                d, ' '.join('%s %.1f/Swap' % (t, c / n) for t, c in threads[sc][d].most_common()),
                ' '.join('%s x%d' % fc for fc in flags[sc][d].most_common(8)),
                ', '.join('%s %.1f' % (k, v / n) for k, v in dirty[sc][d].items())))
        for key in sorted(paths[sc]):
            tot = sum(paths[sc][key].values())
            agg = collections.Counter()
            for bits, c in paths[sc][key].items():
                b = int(bits, 16)
                for i in range(19):
                    if b >> i & 1:
                        agg[i] += c
            labels = ['VS consts', 'PS consts', 'shader load', 're-send', '2200', '2180', '2000', '2100', 'fetch consts', '2280',
                      '2300', '2380', '2388 block', '4900', '>65535', '32-bit idx', 'tiling', 'recording', 'any group']
            print('  %s %s: %.1f calls/Swap; share of calls with: %s' % (
                key[0], key[1], tot / n, ', '.join('%s %.1f%%' % (labels[i], 100.0 * c / tot) for i, c in sorted(agg.items()))))


if __name__ == '__main__':
    main()
