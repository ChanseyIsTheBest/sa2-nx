#!/usr/bin/env python3
"""banjo-nx (2026-10-28, the title update): each generated function's signature, from the PowerPC instructions the
generator writes as comments (`// mnemonic operands`), for mapping a function list of one executable (the base XEX) to
another (the title update's) by content.

Per function (DEFINE_REX_FUNC(sub_XXXXXXXX)), in address order, a list:
- [0] 'shape': a hash of the mnemonics and register operands (immediates, offsets and branch targets left out);
- [1] 'text': a hash of the full text with branch targets and 0x8xxxxxxx addresses blanked: a stricter check;
- [2] 'count': the instruction count;
- [3] 'skel': a hash of the mnemonics only (register allocation left out: a recompile reshuffles it);
- [4] 'calls': the targets of its bl / b instructions (sorted);
- [5] 'hist': how many of each mnemonic (a similarity measure for functions the recompile changed).

  python tools/func_signatures.py <generated dir> <out.json>
See tools/map_functions.py for the mapping.
"""
import glob
import hashlib
import json
import os
import re
import sys


def gen_file(gen, suffix):
    """renut-ultimate (2026-11-19, port 186; found by sasrt-nx's sync): the generator names its files after the project
    (<project>_pch.h, <project>_init.cpp, <project>_recomp.N.cpp): renut_ in the base, a port's own name otherwise."""
    for name in sorted(os.listdir(gen)):
        m = re.fullmatch(r'(\w+)_recomp\.\d+\.cpp', name)
        if m:
            return os.path.join(gen, m.group(1) + '_' + suffix)
    return os.path.join(gen, 'renut_' + suffix)


FUNC = re.compile(r'^DEFINE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\) \{$')
TABLE = re.compile(r'\{ 0x([0-9A-Fa-f]{8}), ([A-Za-z_][A-Za-z0-9_]*) \}')


def addresses(gen_dir):
    """Every function's address by its generated name (renut_init.cpp's table: named functions too)."""
    out = {}
    with open(gen_file(gen_dir, 'init.cpp'), encoding='utf-8', errors='replace') as f:
        for m in TABLE.finditer(f.read()):
            out[m.group(2)] = m.group(1).upper()
    return out
INSN = re.compile(r'^\s*// ([a-z][a-z0-9.+\-]*)(?:\s+(.*))?$')
ADDR = re.compile(r'0x8[0-9a-f]{7}', re.I)
IMM = re.compile(r'(?<![rfv])-?\b(?:0x[0-9a-f]+|\d+)\b', re.I)


class Sig:
    def __init__(self):
        self.shape, self.text, self.skel = hashlib.sha1(), hashlib.sha1(), hashlib.sha1()
        self.count = 0
        self.calls = set()
        self.hist = {}

    def add(self, mnem, ops):
        self.count += 1
        self.hist[mnem] = self.hist.get(mnem, 0) + 1
        blanked = ADDR.sub('A', ops)
        self.text.update((mnem + ' ' + blanked + '\n').encode())
        self.shape.update((mnem + ' ' + IMM.sub('#', blanked) + '\n').encode())
        self.skel.update((mnem + '\n').encode())
        if mnem in ('bl', 'b') and ops.lower().startswith('0x8'):
            self.calls.add(ops[2:10].upper())

    def done(self):
        return [self.shape.hexdigest()[:16], self.text.hexdigest()[:16], self.count, self.skel.hexdigest()[:16],
                sorted(self.calls), self.hist]


def signatures(gen_dir):
    funcs = {}
    by_name = addresses(gen_dir)
    for path in glob.glob(os.path.join(gen_dir, '*.cpp')):
        cur = sig = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = FUNC.match(line)
                if m:
                    if cur:
                        funcs[cur] = sig.done()
                    name = m.group(1)
                    cur = by_name.get(name) or (name[4:] if name.startswith('sub_') else None)
                    sig = Sig()
                    continue
                if cur is None:
                    continue
                i = INSN.match(line)
                if i:
                    sig.add(i.group(1), i.group(2) or '')
        if cur:
            funcs[cur] = sig.done()
    return dict(sorted(funcs.items()))


def main():
    gen_dir, out = sys.argv[1], sys.argv[2]
    funcs = signatures(gen_dir)
    with open(out, 'w') as f:
        json.dump(funcs, f)
    print('%d functions' % len(funcs))


if __name__ == '__main__':
    main()
