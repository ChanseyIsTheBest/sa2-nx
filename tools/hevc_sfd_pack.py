#!/usr/bin/env python3
"""rr6-nx (2026-11-21, the user: "rencoding ... videos as h.265 copy what culdcept saga does"): makes NAME.hevc from a
Sofdec movie NAME (.sfd: an MPEG-1 program stream with MPEG-1 video and ADX audio); sdk/include/rex/filesystem/
sfd_hevc.h has the format and app/src/cpu/rr6_hevc_movies.cpp the hooks that play it. Every byte of the SFD but the
pictures' slice data is kept (packs, packet headers, the audio, the video's sequence, GOP and picture headers and user
data); each picture's slice data becomes a slice header (quantiser 1), a 10-byte tag and zeros. The pictures are
decoded by ffmpeg in display order and encoded as H.265 without B frames (a keyframe every --gop frames, with its
parameter sets), at --fraction of the MPEG-1 video's own bit rate.

  python -P tools/hevc_sfd_pack.py IN.sfd OUT.hevc [--fraction 0.35] [--gop 120] [--encoder libx265|hevc_nvenc]
      [--ffmpeg C:/ffmpeg/ffmpeg.exe]
  python -P tools/hevc_sfd_pack.py --check IN.sfd OUT.hevc [--psnr-every 30]
      the movie the port shows the game (every stored byte as in IN, every tag where a picture's slices begin) and the
      H.265's PSNR against ffmpeg's MPEG-1 decode, every Nth frame
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

MAGIC = b'RXSFDH01'
TAG_BYTES = 10
TAG_MAGIC = bytes([0xA5, 0x5A, 0xC3])
QUANT_BYTE = 0x08  # quantiser_scale 1, extra_bit_slice 0


def make_tag(movie_id, index):
    t = bytearray(TAG_MAGIC)
    for v in (movie_id, index):
        t += bytes([0x80 | ((v >> 14) & 0x7F), 0x80 | ((v >> 7) & 0x7F), 0x80 | (v & 0x7F)])
    t.append(0x80 | (sum(t[3:9]) & 0x7F))
    return bytes(t)


# ---- the program stream ------------------------------------------------------------------------------------------

def demux(d):
    """The video ES and where its bytes lie in the file: es (bytes), runs [(es offset, file offset, length)]."""
    n = len(d)
    i = 0
    es = bytearray()
    runs = []
    while i + 4 <= n:
        if d[i:i + 3] != b'\x00\x00\x01':
            j = d.find(b'\x00\x00\x01', i + 1)
            if j < 0:
                break
            i = j
            continue
        sid = d[i + 3]
        if sid == 0xBA:
            i += 14 + (d[i + 13] & 7) if (d[i + 4] >> 6) == 1 else 12
            continue
        if sid == 0xB9:
            i += 4
            continue
        if sid < 0xBB:
            i += 4
            continue
        ln = struct.unpack('>H', d[i + 4:i + 6])[0] + 6
        if 0xE0 <= sid <= 0xEF:
            p = i + 6
            while d[p] == 0xFF:
                p += 1
            if (d[p] >> 6) == 1:
                p += 2
            f = d[p] >> 4
            if f == 2:
                p += 5
            elif f == 3:
                p += 10
            elif d[p] == 0x0F:
                p += 1
            else:
                raise SystemExit(f'a video packet header at {i} this tool does not read')
            if i + ln > p:
                runs.append((len(es), p, i + ln - p))
                es += d[p:i + ln]
        i += ln
    return bytes(es), runs


def pictures(es):
    """The coded pictures: [(first slice start code, end of its slices, display index)], and the GOP count."""
    starts = []
    p = es.find(b'\x00\x00\x01')
    while p >= 0:
        if p + 3 < len(es):
            starts.append((p, es[p + 3]))
        p = es.find(b'\x00\x00\x01', p + 3)
    pics = []
    gop_base, in_gop, gops = 0, 0, 0
    cur = None
    for s, c in starts:
        if c == 0x00:
            if cur and cur['slice0'] is not None and cur['end'] is None:
                cur['end'] = s
            tref = (es[s + 4] << 2) | (es[s + 5] >> 6)
            cur = {'tref': tref, 'base': gop_base, 'slice0': None, 'end': None}
            pics.append(cur)
            in_gop += 1
        elif 0x01 <= c <= 0xAF:
            if cur and cur['slice0'] is None:
                cur['slice0'] = s
        else:
            if cur and cur['slice0'] is not None and cur['end'] is None:
                cur['end'] = s
            if c == 0xB8:  # a GOP: its pictures' temporal references count from the pictures before it
                gop_base += in_gop
                in_gop = 0
                gops += 1
    if cur and cur['slice0'] is not None and cur['end'] is None:
        cur['end'] = len(es)
    if any(q['slice0'] is None for q in pics):
        raise SystemExit('a picture without slices')
    # sa2-nx-ult (2026-11-21): a picture's display index is its rank among the display times (GOP base + temporal
    # reference): three of SA2's movies (e015bmv, e015mva, gerald35) end on a GOP whose last temporal references skip
    # numbers (209 -> 211), and the decoder still gives every coded picture, in display order.
    times = [q['base'] + q['tref'] for q in pics]
    if len(set(times)) != len(times):
        raise SystemExit('two pictures with the same display time (an unexpected GOP structure)')
    rank = {t: i for i, t in enumerate(sorted(times))}
    out = [(q['slice0'], q['end'], rank[t]) for q, t in zip(pics, times)]
    return out, gops


def es_to_file(runs, es_off, length):
    """The file ranges of ES bytes [es_off, es_off + length)."""
    import bisect
    keys = [r[0] for r in runs]
    k = bisect.bisect_right(keys, es_off) - 1
    out = []
    while length > 0 and k < len(runs):
        e0, f0, ln = runs[k]
        a = max(es_off, e0)
        b = min(es_off + length, e0 + ln)
        if a < b:
            out.append((f0 + a - e0, b - a))
            length -= b - a
            es_off = b
        k += 1
    return out


# ---- H.265 ----------------------------------------------------------------------------------------------------------

def probe(ffmpeg, path):
    ffprobe = os.path.join(os.path.dirname(ffmpeg), 'ffprobe' + os.path.splitext(ffmpeg)[1])
    out = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'stream=width,height,avg_frame_rate', '-of', 'csv=p=0', path],
                         capture_output=True, text=True, timeout=600).stdout.strip().split(',')
    num, den = out[2].split('/')
    return int(out[0]), int(out[1]), float(num) / float(den)


def encode(ffmpeg, src, dst, encoder, bitrate, gop, fps):
    vf = []
    if encoder == 'libx265':
        enc = ['-c:v', 'libx265', '-preset', 'slow', '-b:v', str(bitrate), '-x265-params',
               f'bframes=0:keyint={gop}:min-keyint={gop}:scenecut=0:open-gop=0:repeat-headers=1:aud=1:info=0:'
               f'log-level=error']
    else:
        enc = ['-c:v', 'hevc_nvenc', '-preset', 'p7', '-tune', 'hq', '-rc', 'vbr', '-b:v', str(bitrate), '-maxrate',
               str(bitrate * 2), '-bf', '0', '-g', str(gop), '-forced-idr', '1', '-no-scenecut', '1',
               '-bsf:v', 'hevc_metadata=aud=insert']
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-i', src, '-map', '0:v:0', '-an', '-fps_mode',
           'passthrough', '-pix_fmt', 'yuv420p', *enc, '-f', 'hevc', dst]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=7200)
    if p.returncode:
        raise SystemExit('ffmpeg failed: ' + p.stderr[-2000:])


def access_units(stream):
    """An Annex B H.265 stream cut at its access unit delimiters: [(bytes, keyframe)]."""
    nals = []
    p = stream.find(b'\x00\x00\x01')
    while p >= 0:
        start = p - 1 if p > 0 and stream[p - 1] == 0 else p
        nals.append((start, (stream[p + 3] >> 1) & 0x3F))
        p = stream.find(b'\x00\x00\x01', p + 3)
    aus, cur_start, key = [], None, False
    for k, (start, typ) in enumerate(nals):
        if typ == 35:  # AUD
            if cur_start is not None:
                aus.append([cur_start, start, key])
            cur_start, key = start, False
        if 16 <= typ <= 21:
            key = True
    if cur_start is not None:
        aus.append([cur_start, len(stream), key])
    return [(stream[a:b], k) for a, b, k in aus]


# ---- the .hevc file ------------------------------------------------------------------------------------------------

def build(d, runs, pics, frames, width, height, fps):
    n = len(d)
    kind = np.zeros(n, np.uint8)  # 0 stored, 1 zero, 2 tag
    view = bytearray(d)
    tags = []  # (file offset, coded picture, tag byte index)
    untagged = 0
    for k, (s0, end, disp) in enumerate(pics):
        region = end - (s0 + 4)
        if region < 1 + TAG_BYTES:
            untagged += 1
            continue
        for off, ln in es_to_file(runs, s0 + 4, 1):
            view[off] = QUANT_BYTE
        tb = 0
        for off, ln in es_to_file(runs, s0 + 5, TAG_BYTES):
            for j in range(ln):
                kind[off + j] = 2
                tags.append((off + j, k, tb))
                tb += 1
        for off, ln in es_to_file(runs, s0 + 5 + TAG_BYTES, end - (s0 + 5 + TAG_BYTES)):
            kind[off:off + ln] = 1
    if untagged:
        raise SystemExit(f'{untagged} pictures too small for a tag (not handled)')
    tag_at = {off: (k, tb) for off, k, tb in tags}
    # Run-length segments.
    segs, stored = [], bytearray()
    change = np.flatnonzero(np.diff(kind)) + 1
    bounds = [0, *change.tolist(), n]
    for a, b in zip(bounds[:-1], bounds[1:]):
        kd = int(kind[a])
        if kd == 0:
            segs.append((a, b - a, 0, 0, len(stored)))
            stored += view[a:b]
        elif kd == 1:
            segs.append((a, b - a, 1, 0, 0))
        else:  # split where the picture or the tag's byte order changes
            s = a
            while s < b:
                k, tb = tag_at[s]
                e = s + 1
                while e < b and tag_at[e] == (k, tb + (e - s)):
                    e += 1
                segs.append((s, e - s, 2, k, tb))
                s = e
    hdr = 96
    seg_off = hdr
    stored_off = seg_off + 32 * len(segs)
    pic_off = stored_off + len(stored)
    frame_tab = pic_off + 4 * len(pics)
    data_off = frame_tab + 16 * len(frames)
    out = bytearray()
    out += MAGIC + struct.pack('<IIQQQQIIQIIIIQQ', 1, len(segs), n, seg_off, stored_off, len(stored), len(pics), 0,
                               pic_off, width, height, len(frames), int(round(fps * 1000)), frame_tab, 0)
    assert len(out) == hdr, len(out)
    for s in segs:
        out += struct.pack('<QQIIQ', *s)
    out += stored
    out += b''.join(struct.pack('<I', disp) for _, _, disp in pics)
    pos = data_off
    for f, key in frames:
        out += struct.pack('<QII', pos, len(f), 1 if key else 0)
        pos += len(f)
    for f, _ in frames:
        out += f
    return bytes(out), segs


def parse_hevc(blob):
    if blob[:8] != MAGIC:
        raise SystemExit('not a .hevc movie')
    (ver, nseg, size, seg_off, stored_off, stored_len, npic, _, pic_off, w, h, nfr, fps, frame_tab,
     _) = struct.unpack('<IIQQQQIIQIIIIQQ', blob[8:96])
    segs = [struct.unpack('<QQIIQ', blob[seg_off + 32 * i:seg_off + 32 * i + 32]) for i in range(nseg)]
    pics = struct.unpack(f'<{npic}I', blob[pic_off:pic_off + 4 * npic])
    frames = []
    for i in range(nfr):
        o, sz, fl = struct.unpack('<QII', blob[frame_tab + 16 * i:frame_tab + 16 * i + 16])
        frames.append((blob[o:o + sz], bool(fl & 1)))
    return dict(size=size, segs=segs, stored=blob[stored_off:stored_off + stored_len], pics=pics, w=w, h=h,
                frames=frames, fps=fps / 1000)


def synthesize(o, movie_id=1):
    out = bytearray(o['size'])
    for off, ln, kd, pic, src in o['segs']:
        if kd == 0:
            out[off:off + ln] = o['stored'][src:src + ln]
        elif kd == 2:
            out[off:off + ln] = make_tag(movie_id, o['pics'][pic])[src:src + ln]
    return bytes(out)


def yuv_frames(ffmpeg, path, w, h, fmt=None):
    """The decoded pictures one at a time (I420 bytes), streamed: a long movie does not fit in memory whole."""
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error']
    if fmt:
        cmd += ['-f', fmt]
    cmd += ['-i', path, '-map', '0:v:0', '-fps_mode', 'passthrough', '-pix_fmt', 'yuv420p', '-f', 'rawvideo', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    fs = w * h * 3 // 2
    try:
        while True:
            b = p.stdout.read(fs)
            if len(b) < fs:
                break
            yield b
    finally:
        p.stdout.close()
        p.kill()
        p.wait()


def psnr(a, b):
    e = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if e == 0 else 10 * math.log10(255.0 ** 2 / e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--fraction', type=float, default=0.35)
    ap.add_argument('--gop', type=int, default=120)
    ap.add_argument('--encoder', default='libx265', choices=['libx265', 'hevc_nvenc'])
    ap.add_argument('--ffmpeg', default='C:/ffmpeg/ffmpeg.exe')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--psnr-every', type=int, default=30)
    a = ap.parse_args()
    d = open(a.src, 'rb').read()
    es, runs = demux(d)
    pics, gops = pictures(es)
    if a.check:
        return check(a, d, es, runs, pics)
    width, height, fps = probe(a.ffmpeg, a.src)
    video_bytes = sum(ln for _, _, ln in runs)
    seconds = len(pics) / fps
    bitrate = int(video_bytes * 8 / seconds * a.fraction)
    with tempfile.TemporaryDirectory() as tmp:
        h265 = os.path.join(tmp, 'v.hevc')
        encode(a.ffmpeg, a.src, h265, a.encoder, bitrate, a.gop, fps)
        frames = access_units(open(h265, 'rb').read())
    if len(frames) != len(pics):
        raise SystemExit(f'{len(frames)} H.265 frames for {len(pics)} pictures')
    if not frames[0][1]:
        raise SystemExit('the first H.265 frame is not a keyframe')
    blob, segs = build(d, runs, pics, frames, width, height, fps)
    tmp = a.dst + '.part'
    open(tmp, 'wb').write(blob)
    os.replace(tmp, a.dst)
    h265_bytes = sum(len(f) for f, _ in frames)
    print(f'{os.path.basename(a.src)}: {width}x{height} {fps:.2f} fps, {len(pics)} pictures ({gops} GOPs), MPEG-1 '
          f'{video_bytes / 1e6:.1f} MB at {video_bytes * 8 / seconds / 1e6:.2f} Mbit/s -> H.265 {h265_bytes / 1e6:.1f} MB '
          f'at {h265_bytes * 8 / seconds / 1e6:.2f} Mbit/s ({a.encoder}); {len(d) / 1e6:.1f} MB -> {len(blob) / 1e6:.1f} '
          f'MB, {len(segs)} segments', flush=True)


def check(a, d, es, runs, pics):
    o = parse_hevc(open(a.dst, 'rb').read())
    view = synthesize(o, movie_id=77)
    if len(view) != len(d):
        raise SystemExit('the size differs')
    # Every byte outside the slice data as in the SFD; the slice data as the format says.
    slice_mask = np.zeros(len(d), bool)
    for s0, end, _ in pics:
        for off, ln in es_to_file(runs, s0 + 4, end - (s0 + 4)):
            slice_mask[off:off + ln] = True
    dv, vv = np.frombuffer(d, np.uint8), np.frombuffer(view, np.uint8)
    outside = np.flatnonzero((dv != vv) & ~slice_mask)
    print(f'bytes outside the slice data that differ: {len(outside)}')
    # The tags, read back from the view's ES as the hook reads them (after the slice start code and its header byte).
    es_v, _ = demux(view)
    bad = 0
    for k, (s0, end, disp) in enumerate(pics):
        t = es_v[s0 + 5:s0 + 5 + TAG_BYTES]
        if es_v[s0:s0 + 3] != b'\x00\x00\x01' or es_v[s0 + 4] != QUANT_BYTE or t != make_tag(77, disp):
            bad += 1
    print(f'pictures: {len(pics)}, tags not where they should be: {bad}; the ES zero after the tags: '
          f'{all(not any(es_v[s0 + 5 + TAG_BYTES:end]) for s0, end, _ in pics)}')
    # No start code where there was none: every 00 00 01 in the view's ES is one of the SFD's.
    def codes(b):
        out, p = [], b.find(b'\x00\x00\x01')
        while p >= 0:
            out.append(p)
            p = b.find(b'\x00\x00\x01', p + 3)
        return out
    print(f'start codes: SFD {len(codes(es))}, as shown ' + str(len(codes(es_v))) + ' (the slices after the first in '
          'each picture are gone)')
    w, h = o['w'], o['h']
    with tempfile.TemporaryDirectory() as tmp:
        hv = os.path.join(tmp, 'v.hevc')
        open(hv, 'wb').write(b''.join(f for f, _ in o['frames']))
        vals, n_ref, n_got = [], 0, 0
        ref_it, got_it = yuv_frames(a.ffmpeg, a.src, w, h), yuv_frames(a.ffmpeg, hv, w, h, 'hevc')
        for r, g in zip(ref_it, got_it):
            if n_ref % a.psnr_every == 0:
                vals.append(psnr(np.frombuffer(r, np.uint8), np.frombuffer(g, np.uint8)))
            n_ref += 1
        n_got = n_ref + sum(1 for _ in got_it)
        n_ref += sum(1 for _ in ref_it)
    print(f'frames: MPEG-1 {n_ref}, H.265 {n_got}, keyframes {sum(1 for _, k in o["frames"] if k)}')
    print(f'PSNR every {a.psnr_every}th frame: mean {np.mean(vals):.1f} dB, min {np.min(vals):.1f} dB')


if __name__ == '__main__':
    main()
