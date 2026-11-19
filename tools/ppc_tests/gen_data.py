#!/usr/bin/env python3
"""tw2de_nx (2026-11-01): rexglue recompile-tests' ppc_test_cases.cpp (Catch2 cases) as data for the test runner:
<out>/tests.bin (per test: the function's index, its operations), names.txt, fnlist.cpp (the function table).

  python gen_data.py <recompile-tests output> <out>
"""
import re, struct, sys, os
gen, out = sys.argv[1], sys.argv[2]
cases = open(os.path.join(gen, 'ppc_test_cases.cpp')).read().replace('0x0x', '0x')
parts = re.split(r'\n(?=TEST_CASE\()', cases)
tests = [p for p in parts if p.startswith('TEST_CASE(')]
# kinds: 1 set r u64, 2 set f u64, 3 set f f64, 4 set v lane u32, 5 set mem u8,
#        11 chk r, 12 chk f, 14 chk v lane, 15 chk mem, 16 chk cr
data = bytearray()
names = []
funcs = []
fidx = {}
unknown = set()


def num(s):
    s = s.strip().rstrip('uUlL')
    if s.startswith('[') and s.endswith(']'):
        return int(s[1:-1], 16)
    return int(s, 0)


for t in tests:
    m = re.match(r'TEST_CASE\("([^"]*)"', t)
    names.append(m.group(1))
    call = re.search(r'\n\s*(\w+)\(ctx, memory\);', t)
    fn = call.group(1)
    if fn not in fidx:
        fidx[fn] = len(funcs)
        funcs.append(fn)
    ops = []
    pre, post = t[:call.start()], t[call.end():]
    for line in pre.split('\n'):
        line = line.strip()
        mm = re.match(r'ctx\.r(\d+)\.u64 = (\S+);', line)
        if mm: ops.append((1, int(mm.group(1)), 0, num(mm.group(2)))); continue
        mm = re.match(r'ctx\.f(\d+)\.u64 = (\S+);', line)
        if mm: ops.append((2, int(mm.group(1)), 0, num(mm.group(2)))); continue
        mm = re.match(r'ctx\.f(\d+)\.f64 = (.+);', line)
        if mm:
            ops.append((3, int(mm.group(1)), 0, struct.unpack('<Q', struct.pack('<d', float(mm.group(2))))[0]))
            continue
        mm = re.match(r'ctx\.v(\d+)\.u32\[(\d)\] = (\S+);', line)
        if mm: ops.append((4, int(mm.group(1)), int(mm.group(2)), num(mm.group(3)))); continue
        mm = re.match(r'ctx\.cr0\.set_raw\(\((\S+) >> 28\) & 0xF\);', line)
        if mm: ops.append((7, 0, 0, num(mm.group(1)))); continue
        if re.match(r'ctx\.cr[1-7]\.set_raw\(', line): continue
        mm = re.match(r'ctx\.xer\.so = \((\S+) >> 31\) & 1;', line)
        if mm: ops.append((6, 0, 0, num(mm.group(1)))); continue
        if re.match(r'ctx\.xer\.(ov|ca) = ', line): continue
        mm = re.match(r'memory\[(\S+) \+ (\S+)\] = (\S+);', line)
        if mm: ops.append((5, 0, 0, (num(mm.group(1)) + num(mm.group(2))) << 8 | num(mm.group(3)))); continue
        if line and not line.startswith(('TEST_CASE', 'auto&', 'uint8_t*', 'PPCContext', 'ctx.fpscr')):
            unknown.add(line[:80])
    for line in post.split('\n'):
        line = line.strip()
        mm = re.match(r'REQUIRE\(ctx\.r(\d+)\.u64 == (\S+)\);', line)
        if mm: ops.append((11, int(mm.group(1)), 0, num(mm.group(2)))); continue
        mm = re.match(r'REQUIRE\(ctx\.f(\d+)\.u64 == (\S+)\);', line)
        if mm: ops.append((12, int(mm.group(1)), 0, num(mm.group(2)))); continue
        mm = re.match(r'REQUIRE\(ctx\.f(\d+)\.f64 == (.+)\);', line)
        if mm:
            ops.append((13, int(mm.group(1)), 0, struct.unpack('<Q', struct.pack('<d', float(mm.group(2))))[0]))
            continue
        mm = re.match(r'REQUIRE\(ctx\.v(\d+)\.u32\[(\d)\] == (\S+)\);', line)
        if mm: ops.append((14, int(mm.group(1)), int(mm.group(2)), num(mm.group(3)))); continue
        mm = re.match(r'REQUIRE\(memory\[(\S+) \+ (\S+)\] == (\S+)\);', line)
        if mm: ops.append((15, 0, 0, (num(mm.group(1)) + num(mm.group(2))) << 8 | num(mm.group(3)))); continue
        mm = re.match(r'REQUIRE\(\(\(uint32_t\(ctx\.xer\.so\).*\) == (\S+)\);', line)
        if mm: ops.append((17, 0, 0, num(mm.group(1)))); continue
        mm = re.match(r'REQUIRE\(cr_actual == (\S+)\);', line)
        if mm: ops.append((16, 0, 0, num(mm.group(1)))); continue
        if line and line not in ('}', '{') and not line.startswith('uint32_t cr_actual'):
            unknown.add(line[:80])
    data += struct.pack('<II', fidx[fn], len(ops))
    for k, r, l, v in ops:
        data += struct.pack('<BBBBIQ', k, r, l, 0, 0, v & 0xFFFFFFFFFFFFFFFF)
open(os.path.join(out, 'tests.bin'), 'wb').write(data)
open(os.path.join(out, 'names.txt'), 'w').write('\n'.join(names) + '\n')
with open(os.path.join(out, 'fnlist.cpp'), 'w') as f:
    f.write('#include "ppc_config.h"\n#include <rex/ppc.h>\n')
    f.write(''.join('extern "C" void %s(PPCContext&, uint8_t*);\n' % n for n in funcs))
    f.write('using Fn = void(*)(PPCContext&, uint8_t*);\nFn g_fns[] = {\n' + ''.join('%s,\n' % n for n in funcs) + '};\n')
    f.write('int g_nfns = %d;\n' % len(funcs))
print(len(tests), 'tests', len(funcs), 'functions', len(data), 'bytes; unknown forms:', len(unknown))
for u in sorted(unknown)[:20]:
    print('  ', u)
