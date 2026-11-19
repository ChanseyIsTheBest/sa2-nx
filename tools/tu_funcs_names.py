#!/usr/bin/env python3
"""banjo-nx (2026-10-29, the title update): config/renut_funcs.toml for the title update = reNut upstream's (ab60251,
regenerated for the TU XEX) plus the base executable's names that banjo-nx's sources, lists and tools use, so a name
keeps meaning the same function.

For every base name (the old renut_funcs.toml) that a file under the given roots mentions:
- the TU list has it at the address tools/map_functions.py maps its function to: nothing to do;
- that TU address has no name and the name is free: the name is added there;
- otherwise it is reported (the file's mention must be rewritten, or the name left to fall away from a perf list).
Names that carry a base address (rex_MainFrameLoop_8223C650) are left out: tools/remap_addresses.py rewrites those.
RENAMES: TU entries upstream named for the wrong function (checked in both listings), given their right names.

  python tools/tu_funcs_names.py <old funcs> <TU funcs> <maps dir> <out funcs> <root> [<root> ...]
"""
import json
import os
import re
import sys

ENTRY = re.compile(r'^0x([0-9A-Fa-f]{8})\s*=\s*\{([^}]*)\}(.*)$')
NAME = re.compile(r'name\s*=\s*"([^"]+)"')
WITH_ADDR = re.compile(r'_[0-9A-Fa-f]{8}$|_0x[0-9a-f]{8}$')

# upstream's TU list names the scalar resamplers XAUDIO::SRC::Linear::Process<T,1> as LinearVector (the base list's
# LinearVector::Process<T,1> are 54-instruction wrappers, mapped to 82663CF0 and on; these are 198-instruction loops,
# the base's Linear::Process<T,1> at 0x82671DE0 and on, same lengths).
RENAMES = {
    '8265A170': 'rex_XAUDIO__SRC__Linear__Process_float_1_',
    '8265A458': 'rex_XAUDIO__SRC__Linear__Process_float_2_',
    '8265B420': 'rex_XAUDIO__SRC__Linear__Process_short_1_',
    '8265C978': 'rex_XAUDIO__SRC__Linear__Process_unsigned_char_1_',
    '8265DFF8': 'rex_XAUDIO__SRC__Linear__Process_SHORTLE_1_',
}
# Mappings the call-share check called doubtful, checked by hand (tools/fdiff: same length, same loop, the compiler's
# register and loop-form changes only).
CHECKED = {'8267E990', '8267EC00', '8222A278'}


def read(path):
    by_name, by_addr = {}, {}
    for line in open(path, encoding='utf-8'):
        m = ENTRY.match(line.strip())
        if not m:
            continue
        n = NAME.search(m.group(2))
        a = m.group(1).upper()
        if n:
            by_name[n.group(1)] = a
            by_addr[a] = n.group(1)
    return by_name, by_addr


def mentioned(roots, names):
    found = {}
    pat = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')
    for root in roots:
        paths = [root] if os.path.isfile(root) else [os.path.join(dp, f) for dp, dn, fn in os.walk(root) for f in fn]
        for p in paths:
            q = p.replace(os.sep, '/')
            if '/generated' in q or '__pycache__' in q or '/out/' in q or q.endswith('renut_funcs.toml'):
                continue
            if not q.endswith(('.cpp', '.h', '.txt', '.py', '.toml', '.json', '.inc', '.hpp')):
                continue
            for w in pat.findall(open(p, encoding='utf-8', errors='replace').read()):
                if w in names:
                    found.setdefault(w, set()).add(q)
    return found


def main():
    old_path, tu_path, maps, out_path = sys.argv[1:5]
    roots = sys.argv[5:]
    old_name, _ = read(old_path)
    tu_name, tu_addr = read(tu_path)
    fm = json.load(open(os.path.join(maps, 'func_map.json')))
    manual = os.path.join(maps, 'manual.json')
    if os.path.exists(manual):
        for o, n in json.load(open(manual)).get('funcs', {}).items():
            fm[o.upper()] = [n.upper(), 'manual']
    for a, n in RENAMES.items():
        if a in tu_addr:
            del tu_name[tu_addr[a]]
        tu_addr[a] = n
        tu_name[n] = a
    used = mentioned(roots, old_name)
    add, report = {}, []
    for n, files in sorted(used.items()):
        a = old_name[n]
        m = fm.get(a)
        if WITH_ADDR.search(n):
            continue
        if not m:
            report.append('%-60s %s unmapped%s' % (n, a, ' (TU list has it at %s)' % tu_name[n] if n in tu_name else ''))
            continue
        b, st = m[0], m[1]
        if tu_name.get(n) == b:
            continue
        if st == 'doubtful' and a not in CHECKED:
            report.append('%-60s %s -> %s doubtful: not added' % (n, a, b))
            continue
        if b in tu_addr or n in tu_name:
            report.append('%-60s %s -> %s: TU has %s there%s' % (n, a, b, tu_addr.get(b), ', and the name at %s' %
                                                                  tu_name[n] if n in tu_name else ''))
            continue
        add[b] = n
        tu_addr[b] = n
        tu_name[n] = b
    lines = []
    for line in open(tu_path, encoding='utf-8'):
        m = ENTRY.match(line.strip())
        if m:
            a = m.group(1).upper()
            if a in RENAMES:
                line = '0x%s = {name = "%s"}  # banjo-nx: upstream named it %s\n' % (
                    a, RENAMES[a], NAME.search(m.group(2)).group(1))
            elif a in add:
                body = m.group(2).strip()
                inner = ('name = "%s"' % add[a]) + (', ' + body if body else '')
                line = '0x%s = {%s}  # banjo-nx: base name (tools/tu_funcs_names.py)\n' % (a, inner)
                del add[a]
        lines.append(line)
    if add:
        lines.append('\n# banjo-nx (tools/tu_funcs_names.py): base names of functions the TU list does not list\n')
        for a, n in sorted(add.items()):
            lines.append('0x%s = {name = "%s"}\n' % (a, n))
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(lines)
    print('%d names mentioned; %d added; %d reported' % (len(used), len(used) - len(report), len(report)))
    for r in report:
        print('  ' + r)


if __name__ == '__main__':
    main()
