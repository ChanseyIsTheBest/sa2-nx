// sa2-nx-ult (2026-11-21, the user: "rencode ... all the audio to opus in a web installer like culdcept"): the parts of
// NAME.adxo (sdk/include/rex/filesystem/adx_opus.h has the format) that the PC tool (tools/adx_opus_pack.mjs) and the
// installer's audio worker share: CRI ADX read as SA2's decoder reads it, the tag, the resampler and the file's layout.
// No Node or browser API here: plain ES module, Uint8Array / typed arrays only.

export const BLOCK_BYTES = 18;
export const BLOCK_SAMPLES = 32;
export const HEADER_BYTES = 96;
export const MAGIC = 'RXADXOP1';
export const OPUS_RATE = 48000;

const be16 = (b, o) => (b[o] << 8) | b[o + 1];
const be32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;

// The header of an ADX at `at` of `b` (within `end`), or null when it is not one SA2's decoder plays (encoding 3, 18-byte
// blocks, 4 bits, 1-2 channels, no encryption).
export function parseAdx(b, at = 0, end = b.length) {
  if (end - at < 0x14 || b[at] !== 0x80 || b[at + 1] !== 0x00) return null;
  const copyright = be16(b, at + 2);
  const info = {
    at,
    dataStart: at + copyright + 4,
    encoding: b[at + 4],
    blockBytes: b[at + 5],
    bits: b[at + 6],
    channels: b[at + 7],
    rate: be32(b, at + 8),
    samples: be32(b, at + 12),
    cutoff: be16(b, at + 16),
    version: b[at + 18],
    flags: b[at + 19],
  };
  if (info.encoding !== 3 || info.blockBytes !== BLOCK_BYTES || info.bits !== 4 || info.channels < 1 ||
      info.channels > 2 || info.flags !== 0 || info.rate < 8000 || info.rate > 96000 || info.dataStart > end) {
    return null;
  }
  // The blocks: every frame (a block for each channel) whose blocks' scale fields have the high bit clear, before the
  // end marker (a scale with the high bit set) or the end of the data.
  const frame = BLOCK_BYTES * info.channels;
  let p = info.dataStart;
  let blocks = 0;
  for (; p + frame <= end; p += frame) {
    let ended = false;
    for (let c = 0; c < info.channels; ++c) if (b[p + c * BLOCK_BYTES] & 0x80) ended = true;
    if (ended) break;
    ++blocks;
  }
  info.blocks = blocks;               // a channel's
  info.dataEnd = info.dataStart + blocks * frame;
  info.ended = p + frame <= end;      // an end marker follows
  return info;
}

// CRI's prediction coefficients for the header's high-pass cutoff (FFmpeg's ff_adx_calculate_coeffs; the game's ADXB
// computes the same: checked against its decode step's fields, docs/opus-h265.md).
export function coefficients(cutoff, rate) {
  const a = Math.SQRT2 - Math.cos((2 * Math.PI * cutoff) / rate);
  const b = Math.SQRT2 - 1;
  const c = (a - Math.sqrt((a + b) * (a - b))) / b;
  const lrint = (x) => {
    const f = Math.fround(x);
    const r = Math.round(f);
    return Math.abs(f - Math.trunc(f)) === 0.5 && r % 2 !== 0 ? r - 1 : r;  // to even, as lrintf
  };
  return [lrint(c * 2 * 4096), lrint(-(c * c) * 4096)];
}

const clamp16 = (v) => (v > 32767 ? 32767 : v < -32768 ? -32768 : v);

// The samples SA2's decoder (sub_828CCD28 / sub_828CCE80) gives for the ADX's blocks, every block up to the end marker:
// [Int16Array per channel] of blocks * 32 samples. Its scale is ((field ^ key) & 0x1FFF) + 1, the key 0 (no
// encryption); then s = nibble * scale + ((c1 * s1 + c2 * s2) >> 12), clamped to 16 bits.
export function decodeAdx(b, info) {
  const [c1, c2] = coefficients(info.cutoff, info.rate);
  const out = [];
  for (let c = 0; c < info.channels; ++c) out.push(new Int16Array(info.blocks * BLOCK_SAMPLES));
  const frame = BLOCK_BYTES * info.channels;
  for (let c = 0; c < info.channels; ++c) {
    const o = out[c];
    let s1 = 0, s2 = 0, n = 0;
    for (let k = 0; k < info.blocks; ++k) {
      const p = info.dataStart + k * frame + c * BLOCK_BYTES;
      const scale = (be16(b, p) & 0x1fff) + 1;
      for (let i = 2; i < BLOCK_BYTES; ++i) {
        const byte = b[p + i];
        const hi = (byte << 24) >> 28, lo = (byte << 28) >> 28;
        let s = clamp16(hi * scale + ((c1 * s1 + c2 * s2) >> 12));
        o[n++] = s; s2 = s1; s1 = s;
        s = clamp16(lo * scale + ((c1 * s1 + c2 * s2) >> 12));
        o[n++] = s; s2 = s1; s1 = s;
      }
    }
  }
  return out;
}

// ---- the tag -------------------------------------------------------------------------------------------------------

export function makeTag(id, block, channel) {
  const t = new Uint8Array(BLOCK_BYTES);
  t[0] = 0x7a; t[1] = 0xd5; t[2] = 0xc3;
  t[3] = 0x80 | ((id >> 14) & 0x7f); t[4] = 0x80 | ((id >> 7) & 0x7f); t[5] = 0x80 | (id & 0x7f);
  t[6] = 0x80 | ((block >> 14) & 0x7f); t[7] = 0x80 | ((block >> 7) & 0x7f); t[8] = 0x80 | (block & 0x7f);
  t[9] = 0x80 | channel;
  let sum = 0;
  for (let i = 3; i < 10; ++i) sum += t[i];
  t[10] = 0x80 | (sum & 0x7f);
  return t;
}

export function parseTag(p, o = 0) {
  if (p[o] !== 0x7a || p[o + 1] !== 0xd5 || p[o + 2] !== 0xc3) return null;
  let sum = 0;
  for (let i = 3; i < 10; ++i) {
    if (!(p[o + i] & 0x80)) return null;
    sum += p[o + i];
  }
  if (p[o + 10] !== (0x80 | (sum & 0x7f))) return null;
  for (let i = 11; i < BLOCK_BYTES; ++i) if (p[o + i]) return null;
  return {
    id: ((p[o + 3] & 0x7f) << 14) | ((p[o + 4] & 0x7f) << 7) | (p[o + 5] & 0x7f),
    block: ((p[o + 6] & 0x7f) << 14) | ((p[o + 7] & 0x7f) << 7) | (p[o + 8] & 0x7f),
    channel: p[o + 9] & 0x7f,
  };
}

// ---- the resampler -------------------------------------------------------------------------------------------------
// Zero phase: output sample n is the input at time n * from / to, filtered by a Kaiser-windowed sinc centred on it, so
// there is no delay to undo on either side. Below its Nyquist when it goes down. The console's
// (app/src/sa2_adx_opus.cpp) is the same filter.

const HALF = 16;     // taps each side, in samples of the lower rate
const BETA = 8.0;    // the Kaiser window's
const PASS = 0.97;   // the cut-off, of the lower rate's Nyquist

function bessel0(x) {
  let sum = 1, term = 1;
  for (let k = 1; k < 30; ++k) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
  }
  return sum;
}
const gcd = (a, b) => (b ? gcd(b, a % b) : a);

// The filter's taps for a rate change: one row a phase (den phases), `taps` each, starting at input floor(t) - first.
export function resamplerTaps(from, to) {
  const g = gcd(from, to), num = from / g, den = to / g;  // t = n * num / den
  const scale = Math.min(1, to / from);                    // the lower rate's share of the input's
  const fc = PASS * scale;
  const half = Math.ceil(HALF / scale);                    // input samples each side
  const taps = 2 * half, first = half - 1;
  const i0 = bessel0(BETA);
  const rows = new Float32Array(den * taps);
  for (let p = 0; p < den; ++p) {
    const frac = p / den;
    let sum = 0;
    for (let j = 0; j < taps; ++j) {
      const d = j - first - frac;                          // input k = floor(t) - first + j, d = k - t
      const x = d / (half + 1);
      const w = Math.abs(x) >= 1 ? 0 : bessel0(BETA * Math.sqrt(1 - x * x)) / i0;
      const s = d === 0 ? 1 : Math.sin(Math.PI * fc * d) / (Math.PI * fc * d);
      const v = fc * s * w;
      rows[p * taps + j] = v;
      sum += v;
    }
    for (let j = 0; j < taps; ++j) rows[p * taps + j] /= sum;  // a DC gain of 1 in every phase
  }
  return { num, den, taps, first, rows };
}

// One channel (Int16Array or Float32Array) from `from` Hz to `to` Hz: a Float32Array of ceil(n * to / from) samples.
export function resample(input, from, to) {
  if (from === to) return Float32Array.from(input);
  const f = resamplerTaps(from, to);
  const n = input.length;
  const count = Math.ceil((n * to) / from);
  const out = new Float32Array(count);
  for (let i = 0; i < count; ++i) {
    const q = i * f.num;
    const k0 = Math.floor(q / f.den), p = q - k0 * f.den;
    const row = p * f.taps;
    let acc = 0;
    let k = k0 - f.first;
    for (let j = 0; j < f.taps; ++j, ++k) if (k >= 0 && k < n) acc += input[k] * f.rows[row + j];
    out[i] = acc;
  }
  return out;
}

// ---- the file ------------------------------------------------------------------------------------------------------

// The ADX streams in a game file: a single ADX, or an AFS archive's entries that are ADX. [{ info }] in file order.
export function findStreams(b) {
  const one = parseAdx(b, 0, b.length);
  if (one) return [one];
  if (b.length >= 8 && b[0] === 0x41 && b[1] === 0x46 && b[2] === 0x53 && b[3] === 0) {
    const count = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
    const out = [];
    for (let i = 0; i < count && 8 + 8 * i + 8 <= b.length; ++i) {
      const o = 8 + 8 * i;
      const off = (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
      const size = (b[o + 4] | (b[o + 5] << 8) | (b[o + 6] << 16) | (b[o + 7] << 24)) >>> 0;
      if (!size || off + size > b.length) continue;
      const info = parseAdx(b, off, off + size);
      if (info && info.blocks) out.push(info);
    }
    return out;
  }
  return [];
}

// NAME.adxo from the game file `b`, its streams (findStreams) and each stream's Opus: { packets: [Uint8Array],
// preSkip, bitrate }. A Uint8Array.
export function buildAdxo(b, streams, opus) {
  const n = b.length;
  // Segments: stored bytes between the streams' block data, one kind-3 segment for each stream's blocks.
  const segs = [];
  const storedParts = [];
  let stored = 0, pos = 0;
  const addStored = (a, e) => {
    if (e <= a) return;
    segs.push({ offset: a, length: e - a, kind: 0, stream: 0, source: stored });
    storedParts.push(b.subarray(a, e));
    stored += e - a;
  };
  streams.forEach((s, i) => {
    addStored(pos, s.dataStart);
    segs.push({ offset: s.dataStart, length: s.dataEnd - s.dataStart, kind: 3, stream: i, source: 0 });
    pos = s.dataEnd;
  });
  addStored(pos, n);
  const segOff = HEADER_BYTES;
  const streamOff = segOff + 32 * segs.length;
  let at = streamOff + 64 * streams.length;
  const storedOff = at;
  at += stored;
  const tables = streams.map((s, i) => {
    const t = { tableOff: at };
    at += 4 * opus[i].packets.length;
    t.dataOff = at;
    t.dataLen = opus[i].packets.reduce((a, p) => a + p.length, 0);
    at += t.dataLen;
    return t;
  });
  const out = new Uint8Array(at);
  const dv = new DataView(out.buffer);
  const u64 = (o, v) => { dv.setUint32(o, v % 2 ** 32, true); dv.setUint32(o + 4, Math.floor(v / 2 ** 32), true); };
  for (let i = 0; i < MAGIC.length; ++i) out[i] = MAGIC.charCodeAt(i);
  dv.setUint32(8, 1, true);
  dv.setUint32(12, segs.length, true);
  u64(16, n);
  u64(24, segOff);
  u64(32, storedOff);
  u64(40, stored);
  dv.setUint32(48, streams.length, true);
  u64(56, streamOff);
  segs.forEach((s, k) => {
    const o = segOff + 32 * k;
    u64(o, s.offset);
    u64(o + 8, s.length);
    dv.setUint32(o + 16, s.kind, true);
    dv.setUint32(o + 20, s.stream, true);
    u64(o + 24, s.source);
  });
  streams.forEach((s, i) => {
    const o = streamOff + 64 * i;
    dv.setUint32(o, s.channels, true);
    dv.setUint32(o + 4, s.rate, true);
    dv.setUint32(o + 8, s.blocks, true);
    dv.setUint32(o + 12, opus[i].packets.length, true);
    dv.setUint16(o + 16, opus[i].preSkip, true);
    u64(o + 24, tables[i].tableOff);
    u64(o + 32, tables[i].dataOff);
    u64(o + 40, tables[i].dataLen);
    dv.setUint32(o + 48, opus[i].bitrate || 0, true);
  });
  let w = storedOff;
  for (const part of storedParts) { out.set(part, w); w += part.length; }
  streams.forEach((s, i) => {
    let t = tables[i].tableOff, d = tables[i].dataOff;
    for (const p of opus[i].packets) {
      dv.setUint32(t, p.length, true);
      t += 4;
      out.set(p, d);
      d += p.length;
    }
  });
  return out;
}

// The game file a NAME.adxo stands for, made as the console makes it (tags with stream ids `ids[i]`), and its streams'
// Opus: { file: Uint8Array, streams: [{ channels, rate, blocks, preSkip, packets: [Uint8Array] }] }.
export function readAdxo(a, ids = null) {
  const dv = new DataView(a.buffer, a.byteOffset, a.byteLength);
  const u64 = (o) => dv.getUint32(o, true) + dv.getUint32(o + 4, true) * 2 ** 32;
  for (let i = 0; i < MAGIC.length; ++i) if (a[i] !== MAGIC.charCodeAt(i)) throw new Error('not an .adxo file');
  const nseg = dv.getUint32(12, true), size = u64(16), segOff = u64(24), storedOff = u64(32);
  const nstr = dv.getUint32(48, true), strOff = u64(56);
  const streams = [];
  for (let i = 0; i < nstr; ++i) {
    const o = strOff + 64 * i;
    const s = { channels: dv.getUint32(o, true), rate: dv.getUint32(o + 4, true), blocks: dv.getUint32(o + 8, true),
                preSkip: dv.getUint16(o + 16, true), packets: [] };
    const np = dv.getUint32(o + 12, true);
    let t = u64(o + 24), d = u64(o + 32);
    for (let k = 0; k < np; ++k) {
      const len = dv.getUint32(t + 4 * k, true);
      s.packets.push(a.subarray(d, d + len));
      d += len;
    }
    streams.push(s);
  }
  const file = new Uint8Array(size);
  for (let k = 0; k < nseg; ++k) {
    const o = segOff + 32 * k;
    const off = u64(o), len = u64(o + 8), kind = dv.getUint32(o + 16, true), st = dv.getUint32(o + 20, true);
    const src = u64(o + 24);
    if (kind === 0) file.set(a.subarray(storedOff + src, storedOff + src + len), off);
    else if (kind === 3) {
      const s = streams[st];
      const id = ids ? ids[st] : st + 1;
      for (let q = 0; q < len; ++q) {
        const byte = src + q, blk = Math.floor(byte / BLOCK_BYTES), inside = byte % BLOCK_BYTES;
        const tag = makeTag(id, Math.floor(blk / s.channels), blk % s.channels);
        file[off + q] = tag[inside];
      }
    }
  }
  return { file, streams };
}
