# tw2de_nx (2026-11-03): a model of Xenia's x64 vupkd3d128 sequences (src/xenia/cpu/backend/x64/x64_seq_vector.cc, BSD:
# ../XENIA_LICENSE), lane i = guest element i, and the test file it gives.
import struct
M=0xFFFFFFFF
def tobytes(l): return b''.join(struct.pack('<I',x&M) for x in l)
def frombytes(b): return [struct.unpack_from('<I',b,4*i)[0] for i in range(4)]
def pshufb(v,c):
    b=tobytes(v); cb=tobytes(c)
    return frombytes(bytes(0 if cb[i]&0x80 else b[cb[i]&15] for i in range(16)))
def slld(v,n): return [(x<<n)&M for x in v]
def srld(v,n): return [(x&M)>>n for x in v]
def srad(v,n): return [((x-(1<<32) if x&0x80000000 else x)>>n)&M for x in v]
def addd(v,c): return [(a+b)&M for a,b in zip(v,c)]
def andd(v,c): return [a&b for a,b in zip(v,c)]
def shufps(a,b,z,y,x,w): return [a[w],a[x],b[y],b[z]]
def f(u): return struct.unpack('<f',struct.pack('<I',u))[0]
def nanify(v,ovf):
    import math
    return [0x7FC00000 if (not math.isnan(f(x)) and f(x)==f(ovf)) else x for x in v]
C3301=[0x40400000,0x40400000,0,0x3F800000]; C3333=[0x40400000]*4; C3331=[0x40400000]*3+[0x3F800000]
def short2(v): d=pshufb(v,[0xFFFF0F0E,0xFFFF0D0C,0xFFFFFFFF,0xFFFFFFFF]); d=srad(slld(d,16),16); d=addd(d,C3301); return nanify(d,0x403F8000)
def short4(v): d=pshufb(v,[0xFFFF0B0A,0xFFFF0908,0xFFFF0F0E,0xFFFF0D0C]); d=srad(slld(d,16),16); d=addd(d,C3333); return nanify(d,0x403F8000)
def u2101010(v):
    d=[v[3]]*4; d=andd(d,[0x3FF,0x3FF<<10,0x3FF<<20,3<<30]); d=[x>>s for x,s in zip(d,[0,10,20,30])]
    d=srad(slld(d,22),22); d=addd(d,C3331); return nanify(d,0x403FFE00)
def u4202020(v):
    d=pshufb(v,[0xFF0E0D0C,0xFF0B0A09,0xFF080F0E,0xFFFFFF0B]); x0=srld(d,4)
    d=shufps(d,x0,3,2,1,0); d=shufps(d,d,3,1,2,0); d=srad(slld(d,12),12); d=addd(d,C3331); return nanify(d,0x40380000)

# ---- the test file (python gen_vupkd3d128_nan.py > instr_own_vupkd3d128_nan.s)
if __name__ == '__main__':
    V = lambda ws: '[' + ', '.join('%08X' % w for w in ws) + ']'
    out = ['# tw2de_nx (2026-11-03): vupkd3d128\'s "most negative field is a quiet NaN" (NORMSHORT2 -32768, NORMSHORT4 -32768,',
           '# NORMPACKED32 -512 in x, y and z, NORMPACKED64 -524288 in x, y, z) and the NORMPACKED64 decode (the low',
           '# doubleword, signed fields, 3.0+X / 1.0+W bit-pattern form). Expectations from a model of Xenia\'s x64 sequences',
           '# (EmitSHORT_2 / EmitSHORT_4 / EmitUINT_2101010 / EmitULONG_4202020: shuffle, sign extend, add the bias, compare with',
           '# the overflow constant, blend QNaN), tools/ppc_tests/own/gen_vupkd3d128_nan.py. The immediate is the type * 4.', '']
    cases = [('short2_min', 1, [0x11111111, 0x22222222, 0x33333333, 0x80008000], short2),
             ('short2_mixed', 1, [0, 0, 0, 0x80007FFF], short2),
             ('short2_aliased_min', 1, [0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x00018000], short2),
             ('short4_min', 4, [0, 0, 0x80000001, 0xFFFF8000], short4),
             ('short4_plain', 4, [0xCDCDCDCD, 0xCDCDCDCD, 0x7FFFFFFF, 0x007FFFF8], short4),
             ('packed32_min_y', 2, [0, 0, 0, (0x200 << 10) | 5], u2101010),
             ('packed32_min_z', 2, [0, 0, 0, (0x200 << 20) | (3 << 30) | 0x1FF], u2101010),
             ('packed32_min_xyz', 2, [0, 0, 0, 0x200 | (0x200 << 10) | (0x200 << 20)], u2101010),
             ('packed64_plain', 6, [0xDEADBEEF, 0xCAFEF00D, 0x50000300, 0x00400001], u4202020),
             ('packed64_min_x', 6, [0, 0, 0x00000000, 0x00080000], u4202020),
             ('packed64_min_yz', 6, [0, 0, 0x08000080, 0x00000000], u4202020)]
    for name, t, vin, fn in cases:
        exp = fn(vin)
        out += ['test_vupkd3d128_%s:' % name, '  #_ REGISTER_IN v4 %s' % V(vin), '  vupkd3d128 v3, v4, %d' % (t * 4), '  blr',
                '  #_ REGISTER_OUT v3 %s' % V(exp), '']
        if name.startswith('short2_aliased'):
            out += ['test_vupkd3d128_%s_same:' % name, '  #_ REGISTER_IN v3 %s' % V(vin), '  vupkd3d128 v3, v3, %d' % (t * 4),
                    '  blr', '  #_ REGISTER_OUT v3 %s' % V(exp), '']
    print('\n'.join(out))
