#!/usr/bin/env python3
"""banjo-nx (2026-10-20): one page of what a console (or emulator) renut log says about the package-36 questions.

Usage: python tools/summarise_console_log.py <renut_NNN.log> [more logs]

Per log: the fps and GPU ms of each 10 s report (the heaviest periods first), the in-run A/B halves ("[renut A/B]",
light branch 15 / gpr32 16 and the others), the [latency] lines (input -> present, input -> GPU done, late input,
latency trim), the stale texture headers ("[flash] device swizzle", "header vs device"), fur shells drawn / skipped,
and the shaders compiled in play ("uam N compiles", "run-time variant" lines). Plain text, for pasting into a reply."""
import re
import sys

RE_PERIOD = re.compile(r'\[renut\] ([0-9.]+) s: (\d+) game frames \(([0-9.]+) fps\), render thread busy ([0-9.]+) ms a '
                       r'frame \((\d+) frames\), GPU ([0-9.]+) ms a frame')
RE_AB = re.compile(r'\[renut A/B\] (.*?) \(target (\d+)\): on ([0-9.]+) fps, render thread ([0-9.]+) ms a frame, GPU '
                   r'([0-9.]+) ms a frame \(([0-9]+) s\); off ([0-9.]+) fps, render thread ([0-9.]+) ms a frame, GPU '
                   r'([0-9.]+) ms a frame \(([0-9]+) s\)')
RE_LAT = re.compile(r'input -> present ([0-9.]+) \(max ([0-9.]+)\); (\d+) frames(?:; input -> GPU done ([0-9.]+) ms '
                    r'\(max ([0-9.]+), \d+ frames\))?; late input: (.*)$')
RE_TRIM = re.compile(r'latency trim \(banjo_deko3d_latency_trim\): (\d+) of (\d+) frames held back ([0-9.]+) ms on '
                     r'average; the GPU ran dry after one (\d+) times')
RE_SWZ = re.compile(r'device swizzle: (\d+) binds in (\d+) frames')
RE_HDR = re.compile(r'header vs device \(same data\): (\d+) binds .*?0 \(signs, pitch, tiling\) (\d+), 3 \(swizzle, '
                    r'number format, exponent\) (\d+), 4 \(mip levels\) (\d+), 5 \(mips\) (\d+)')
RE_FUR = re.compile(r'fur shells \((.*?)\): (\d+) drawn, (\d+) not drawn')
RE_SKIN = re.compile(r'skin once \(banjo_deko3d_skin_once\): (\d+) fur draws skinned and wrote their rows, (\d+) read them, '
                     r'(\d+) skinned as before')
RE_SKIN2 = re.compile(r'other skinned draws \(shadow maps, depth only\): (\d+) wrote, (\d+) read')
RE_UAM = re.compile(r'uam (\d+) compiles \(avg (\d+) ms, (\d+) failed\), DKSH cache (\d+) hits, (\d+) added')
RE_RTV = re.compile(r'run-time variant \(stage (\d+), (\d+) bytes(?:, base \d+ bytes [0-9a-f]{16})?\): (.*)$')


def mean(v):
    return sum(v) / len(v) if v else 0.0


def summarise(path):
    periods, ab, lat, trim, fur = [], {}, [], [], []
    skin = [0, 0, 0, 0, 0]
    swz = [0, 0]
    hdr = [0, 0, 0, 0, 0]
    uam_last, rtv = None, []
    for ln in open(path, encoding='utf-8', errors='replace'):
        if (m := RE_PERIOD.search(ln)):
            periods.append((float(m[3]), float(m[4]), float(m[6])))
        elif (m := RE_AB.search(ln)):
            a = ab.setdefault((m[2], m[1]), [[], [], [], []])
            a[0].append(float(m[3])), a[1].append(float(m[5])), a[2].append(float(m[7])), a[3].append(float(m[9]))
        elif (m := RE_TRIM.search(ln)):
            trim.append(tuple(float(x) for x in m.groups()))
        elif (m := RE_LAT.search(ln)):
            lat.append((float(m[1]), float(m[2]), float(m[4] or 0), float(m[5] or 0), m[6]))
        elif (m := RE_SWZ.search(ln)):
            swz[0] += int(m[1]); swz[1] += int(m[2])
        elif (m := RE_HDR.search(ln)):
            for i in range(5):
                hdr[i] += int(m[i + 1])
        elif (m := RE_FUR.search(ln)):
            fur.append((m[1], int(m[2]), int(m[3])))
        elif (m := RE_SKIN.search(ln)):
            for i in range(3):
                skin[i] += int(m[i + 1])
            if (m2 := RE_SKIN2.search(ln)):
                skin[3] += int(m2[1])
                skin[4] += int(m2[2])
        elif (m := RE_UAM.search(ln)):
            uam_last = m.groups()
        elif (m := RE_RTV.search(ln)):
            # the variant's own constants come first, in ascending ids; the shader's defaults follow
            first, last = [], -1
            for item in [x.strip() for x in m[3].split(';') if x.strip()]:
                cid = int(item.split()[0]) if item.split()[0].isdigit() else -1
                if cid <= last:
                    break
                last = cid
                first.append(item.replace(' ', '=').rstrip('u'))
            rtv.append(f'stage {m[1]}, {m[2]} bytes: {" ".join(first)[:160]}')
    out = [f'== {path}']
    if periods:
        heavy = sorted(periods, key=lambda p: -p[2])[:5]
        out.append(f'periods {len(periods)}: mean {mean([p[0] for p in periods]):.1f} fps, GPU {mean([p[2] for p in periods]):.1f} '
                   f'ms, render thread {mean([p[1] for p in periods]):.1f} ms; lowest fps {min(p[0] for p in periods):.1f}')
        out.append('heaviest GPU periods: ' + '; '.join(f'{g:.1f} ms GPU at {f:.1f} fps' for f, _, g in heavy))
    for (target, name), a in sorted(ab.items()):
        on_gpu, off_gpu = mean(a[1]), mean(a[3])
        out.append(f'A/B {name} (target {target}), {len(a[1])} lines: on {mean(a[0]):.1f} fps / GPU {on_gpu:.2f} ms, '
                   f'off {mean(a[2]):.1f} fps / GPU {off_gpu:.2f} ms ({(on_gpu - off_gpu) / off_gpu * 100 if off_gpu else 0:+.1f} % '
                   f'GPU with it on)')
    if lat:
        out.append(f'latency: input -> present {mean([l[0] for l in lat]):.1f} ms (worst max {max(l[1] for l in lat):.1f}); '
                   f'input -> GPU done {mean([l[2] for l in lat if l[2]]):.1f} ms (worst max {max(l[3] for l in lat):.1f}); '
                   f'late input: {lat[-1][4][:90]}')
    if trim:
        out.append(f'latency trim: {int(sum(t[0] for t in trim))} of {int(sum(t[1] for t in trim))} frames held back, '
                   f'{mean([t[2] for t in trim if t[0]]):.1f} ms mean hold; GPU ran dry {int(sum(t[3] for t in trim))} times')
    out.append(f'stale headers: {swz[0]} binds in {swz[1]} frames through the device swizzle; same-data dword diffs: '
               f'0={hdr[1]} 3={hdr[2]} 4={hdr[3]} (by design) 5={hdr[4]}')
    if fur:
        out.append(f'fur shells ({fur[-1][0]}): {sum(f[1] for f in fur)} drawn, {sum(f[2] for f in fur)} not drawn')
    if any(skin):
        out.append(f'skin once: {skin[0]} fur draws wrote rows, {skin[1]} read them, {skin[2]} skinned as before; '
                   f'other skinned draws {skin[3]} wrote, {skin[4]} read')
    if uam_last:
        out.append(f'shader compiles: uam {uam_last[0]} (avg {uam_last[1]} ms, {uam_last[2]} failed), cache {uam_last[3]} '
                   f'hits, {uam_last[4]} added')
    for r in rtv[:20]:
        out.append('  run-time variant ' + r)
    return '\n'.join(out)


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(summarise(p))
        print()
