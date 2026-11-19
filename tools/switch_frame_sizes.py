import re
import subprocess
import sys

# Stack frame sizes (from each function's first instructions) of renut::gpu / plume functions in two ELFs; prints the
# ones that grew by more than a threshold. Usage: frames.py old.elf new.elf [min_growth_bytes]
NM = r'C:/devkitPro/devkitA64/bin/aarch64-none-elf-nm.exe'
OD = r'C:/devkitPro/devkitA64/bin/aarch64-none-elf-objdump.exe'


def frames(elf):
    syms = subprocess.run([NM, '-n', '-C', elf], capture_output=True, text=True).stdout.splitlines()
    funcs = []
    for line in syms:
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in ('t', 'T') and ('renut::gpu' in parts[2] or 'plume::' in parts[2]):
            funcs.append((int(parts[0], 16), parts[2]))
    lo = min(a for a, _ in funcs)
    hi = max(a for a, _ in funcs) + 64
    dis = subprocess.run([OD, '-d', '--no-show-raw-insn', '--start-address=0x%x' % lo, '--stop-address=0x%x' % hi, elf],
                         capture_output=True, text=True).stdout.splitlines()
    insns = {}
    for line in dis:
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
        if m:
            insns[int(m.group(1), 16)] = m.group(2)
    out = {}
    for a, name in funcs:
        size = 0
        movs = {}
        for k in range(12):
            t = insns.get(a + 4 * k, '')
            m = re.match(r'mov\s+(x\d+), #0x([0-9a-f]+)', t)
            if m:
                movs[m.group(1)] = int(m.group(2), 16)
            m = re.match(r'sub\s+sp, sp, #0x([0-9a-f]+)(, lsl #12)?', t)
            if m:
                size += int(m.group(1), 16) << (12 if m.group(2) else 0)
            m = re.match(r'sub\s+sp, sp, (x\d+)', t)
            if m and m.group(1) in movs:
                size += movs[m.group(1)]
            m = re.match(r'stp\s+x29, x30, \[sp, #-(\d+)\]!', t)
            if m:
                size += int(m.group(1))
        out[name] = max(out.get(name, 0), size)
    return out


old, new = frames(sys.argv[1]), frames(sys.argv[2])
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 2048
for name in sorted(new, key=lambda n: new[n] - old.get(n, 0), reverse=True):
    grow = new[name] - old.get(name, 0)
    if grow > limit:
        print('%7d -> %7d  %s' % (old.get(name, 0), new[name], name[:120]))
print('functions', len(new))
