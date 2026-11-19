#!/usr/bin/env python3
"""katamari-nx (2026-11-02): where the web installer finds Beautiful Katamari's shaders in the player's game files.

111 of the 195 shaders are containers stored in default.xex; the other 84 are in the models of d02.dat-d05.dat, inside
CPDB blocks (tools/cpdb.py, tools/katamari_files.py). Decoding all 5.9 GB of them in a browser is not needed: this lists
few records (archive, offset, sizes) that hold those 84, the smallest record for each, and where each container is
inside its decoded record. The list holds numbers and hashes only, no game data: it goes into the installer's
release/manifest.json (katamari-nx-installer's test/release_node.mjs).

  python tools/katamari_shader_files.py --image out/renut_image.bin --game <game folder> --wanted out/renut_shader_dump \
      --out out/katamari_shader_files.json
"""
import argparse
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import katamari_files as kf  # noqa: E402
from renut_shader_dump import find_shader_headers  # noqa: E402


def main():
    import xxhash
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--wanted', required=True, help='the folder of every shader of the build (out/renut_shader_dump)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    wanted = {n[:-4].lower() for n in os.listdir(a.wanted) if n.endswith('.bin')}
    image = open(a.image, 'rb').read()
    in_image = {}
    for h in find_shader_headers(image):
        virt, phys = struct.unpack_from('>II', image, h + 4)
        blob = image[h:h + virt + phys]
        if len(blob) == virt + phys:
            kind = 'vs' if struct.unpack_from('>I', image, h)[0] & 1 else 'ps'
            in_image.setdefault(f'{kind}_{xxhash.xxh3_64_hexdigest(blob)}', hashlib.sha256(blob).hexdigest())
    rest = wanted - set(in_image)
    print(f'{len(wanted)} shaders: {len(wanted & set(in_image))} in the image, {len(rest)} to find in the files')
    # Every record that holds one of them: (stored size, archive, offset, flags, size, [(name, at, length, sha256)]).
    found = []
    handles = {}
    seen = set()
    for name, table, table2, count in kf.categories(image):
        for t in (table, table2):
            if not t:
                continue
            for _, archive, flags, size, offset, stored in kf.records(image, t, count):
                if not archive.endswith('.dat') or (archive, offset) in seen:
                    continue
                seen.add((archive, offset))
                try:
                    data = kf.read_file(a.game, archive, flags, size, offset, stored, handles)
                except Exception:  # noqa: BLE001  (the files the game never loads: tools/katamari_files.py counts them)
                    continue
                here = []
                for h in find_shader_headers(data, aligned=False):
                    virt, phys = struct.unpack_from('>II', data, h + 4)
                    blob = data[h:h + virt + phys]
                    if len(blob) != virt + phys:
                        continue
                    kind = 'vs' if struct.unpack_from('>I', data, h)[0] & 1 else 'ps'
                    n = f'{kind}_{xxhash.xxh3_64_hexdigest(blob)}'
                    if n in rest:
                        here.append((n, h, len(blob), hashlib.sha256(blob).hexdigest()))
                if here:
                    found.append((stored, archive, offset, flags, size, here))
    # Greedy: the records that cover the most still-missing shaders per stored byte, until none is missing.
    missing = set(rest)
    chosen = []
    while missing:
        best = None
        for r in found:
            gain = len({c[0] for c in r[5]} & missing)
            if gain and (best is None or gain / r[0] > best[0]):
                best = (gain / r[0], r)
        if best is None:
            sys.exit(f'{len(missing)} shaders are in no record, e.g. {sorted(missing)[:3]}')
        r = best[1]
        take = {}
        for c in r[5]:
            if c[0] in missing and c[0] not in take:
                take[c[0]] = c
        missing -= set(take)
        chosen.append({'archive': r[1], 'offset': r[2], 'stored': r[0], 'compressed': bool(r[3] & 0b010), 'size': r[4],
                       'containers': [{'at': c[1], 'length': c[2], 'sha256': c[3]} for c in take.values()]})
    chosen.sort(key=lambda r: (r['archive'], r['offset']))
    out = {'image_sha256': sorted(v for k, v in in_image.items() if k in wanted), 'files': chosen}
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(out, f, indent=1)
    stored = sum(r['stored'] for r in chosen)
    print(f'{len(chosen)} records hold the {len(rest)}: {stored / 1e6:.1f} MB to read, '
          f'{sum(r["size"] for r in chosen) / 1e6:.1f} MB decoded -> {a.out}')


if __name__ == '__main__':
    sys.setrecursionlimit(10000)
    main()
