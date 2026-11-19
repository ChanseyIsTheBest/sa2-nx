#!/usr/bin/env python3
"""pacman-nx (2026-11-02): writes the generator's non_argument_as_local_keep list from tools/non_argument_audit.py's
report on code generated WITHOUT the option: every sub_X of an ENTRY line (it reads a register a caller passes there),
every sub_X of a RET line and the callee that line names (the value crosses the call; both must keep it in ctx).
The helpers (__savegprlr, __restvmx ...) are kept by name in the generator. Prints the TOML lines.

  python tools/diet_keep_list.py out/nonarg1.log >> app/renut_config.toml
"""
import re
import sys

entry, ret = set(), set()
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.match(r'(ENTRY|RET) (sub_[0-9A-F]{8}) ', line)
    if not m:
        continue
    (entry if m.group(1) == 'ENTRY' else ret).add(int(m.group(2)[4:], 16))
    # renut-ultimate (2026-11-20, port 194; TFU-nx's of 2026-11-12, via S4ep1-nx): every callee the line names, not
    # only the first ("<= sub_A sub_B", "<= INDIRECT sub_A").
    c = line.split('<=', 1)
    if len(c) == 2:
        for callee in re.findall(r'sub_([0-9A-F]{8})', c[1]):
            ret.add(int(callee, 16))
all_ = sorted(entry | ret)
print(f'# {len(entry)} ENTRY and {len(ret)} RET functions (tools/diet_keep_list.py, from tools/non_argument_audit.py).')
print('non_argument_as_local_keep = [')
for i in range(0, len(all_), 8):
    print('    ' + ', '.join(f'0x{a:08X}' for a in all_[i:i + 8]) + ',')
print(']')
