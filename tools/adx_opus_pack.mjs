#!/usr/bin/env node
// sa2-nx-ult (2026-11-21, the user: "rencode ... all the audio to opus in a web installer like culdcept"): makes NAME.adxo
// (sdk/include/rex/filesystem/adx_opus.h) from a game file NAME that holds CRI ADX streams (an .adx, an .afs of them),
// on a PC, for tests; the installer's audio worker makes the same files in the browser (WebCodecs' Opus encoder in place
// of FFmpeg's libopus). Each stream: SA2's decode of its blocks (tools/adx_opus_lib.mjs), resampled to 48 kHz, encoded
// as Opus (20 ms packets), checked by decoding the Opus back to the stream's rate (the SNR against the game's decode).
//
//   node tools/adx_opus_pack.mjs IN OUT.adxo [--stereo 128] [--mono 40] [--ffmpeg C:/ffmpeg/ffmpeg.exe] [--quiet]
//   node tools/adx_opus_pack.mjs --check IN OUT.adxo
//       the file the console shows the game (every byte but the blocks as in IN, every block a tag of its stream,
//       channel and index)
import fs from 'node:fs';
import { spawnSync } from 'node:child_process';
import * as L from './adx_opus_lib.mjs';

function args() {
  const a = process.argv.slice(2);
  const o = { stereo: 128, mono: 40, ffmpeg: 'C:/ffmpeg/ffmpeg.exe', check: false, quiet: false, files: [] };
  for (let i = 0; i < a.length; ++i) {
    if (a[i] === '--stereo') o.stereo = Number(a[++i]);
    else if (a[i] === '--mono') o.mono = Number(a[++i]);
    else if (a[i] === '--ffmpeg') o.ffmpeg = a[++i];
    else if (a[i] === '--check') o.check = true;
    else if (a[i] === '--quiet') o.quiet = true;
    else o.files.push(a[i]);
  }
  if (o.files.length !== 2) {
    console.error('usage: node tools/adx_opus_pack.mjs [--check] IN OUT.adxo [--stereo kbit/s] [--mono kbit/s]');
    process.exit(2);
  }
  return o;
}

// Ogg pages -> packets (RFC 3533 lacing).
function oggPackets(b) {
  const packets = [];
  let cur = [];
  let p = 0;
  while (p + 27 <= b.length) {
    if (b[p] !== 0x4f || b[p + 1] !== 0x67 || b[p + 2] !== 0x67 || b[p + 3] !== 0x53) throw new Error('bad Ogg page');
    const nseg = b[p + 26];
    let d = p + 27 + nseg;
    for (let i = 0; i < nseg; ++i) {
      const len = b[p + 27 + i];
      cur.push(b.subarray(d, d + len));
      d += len;
      if (len < 255) {
        const total = cur.reduce((s, x) => s + x.length, 0);
        const pk = new Uint8Array(total);
        let w = 0;
        for (const x of cur) { pk.set(x, w); w += x.length; }
        packets.push(pk);
        cur = [];
      }
    }
    p = d;
  }
  return packets;
}

function toS16(planar) {
  const ch = planar.length, n = planar[0].length;
  const out = new Int16Array(n * ch);
  for (let i = 0; i < n; ++i)
    for (let c = 0; c < ch; ++c) {
      const v = Math.round(planar[c][i]);
      out[i * ch + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
  return Buffer.from(out.buffer);
}

function encodeOpus(o, pcm48, channels, kbps) {
  const r = spawnSync(o.ffmpeg, ['-hide_banner', '-loglevel', 'error', '-f', 's16le', '-ar', '48000', '-ac', String(channels),
    '-i', '-', '-c:a', 'libopus', '-b:a', `${kbps}k`, '-vbr', 'on', '-frame_duration', '20', '-application', 'audio',
    '-f', 'ogg', '-'], { input: toS16(pcm48), maxBuffer: 1 << 30 });
  if (r.status) throw new Error('ffmpeg (encode) failed: ' + r.stderr.toString().slice(-500));
  const pk = oggPackets(new Uint8Array(r.stdout.buffer, r.stdout.byteOffset, r.stdout.length));
  const head = pk[0];
  if (String.fromCharCode(...head.subarray(0, 8)) !== 'OpusHead') throw new Error('no OpusHead');
  const preSkip = head[10] | (head[11] << 8);
  return { ogg: r.stdout, packets: pk.slice(2), preSkip, bitrate: kbps * 1000 };
}

// The Opus decoded back (FFmpeg skips the pre-skip), at 48 kHz: planar Float32Array per channel.
function decodeOgg(o, ogg, channels) {
  const r = spawnSync(o.ffmpeg, ['-hide_banner', '-loglevel', 'error', '-f', 'ogg', '-i', '-', '-f', 's16le', '-ar', '48000',
    '-ac', String(channels), '-'], { input: ogg, maxBuffer: 1 << 30 });
  if (r.status) throw new Error('ffmpeg (decode) failed: ' + r.stderr.toString().slice(-500));
  const s = new Int16Array(r.stdout.buffer, r.stdout.byteOffset, r.stdout.length >> 1);
  const n = s.length / channels;
  const out = [];
  for (let c = 0; c < channels; ++c) {
    const ch = new Float32Array(n);
    for (let i = 0; i < n; ++i) ch[i] = s[i * channels + c];
    out.push(ch);
  }
  return out;
}

// Opus keeps each band's energy, not the waveform (its full-band SNR against the source is 15-20 dB at any bit rate),
// so the check is the alignment and the waveform below 3 kHz: both signals low-passed (a 255-tap windowed sinc), the
// SNR at the best lag within +-32 samples, which must be 0 (the console takes sample n from n / rate).
function lowpass(x, rate, hz = 3000) {
  const half = 127, fc = hz / rate;
  const taps = new Float32Array(2 * half + 1);
  let sum = 0;
  for (let j = -half; j <= half; ++j) {
    const w = 0.42 + 0.5 * Math.cos((Math.PI * j) / (half + 1)) + 0.08 * Math.cos((2 * Math.PI * j) / (half + 1));
    const v = (j === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * j) / (Math.PI * j)) * w;
    taps[j + half] = v;
    sum += v;
  }
  const out = new Float32Array(x.length);
  for (let i = 0; i < x.length; ++i) {
    let acc = 0;
    for (let j = -half; j <= half; ++j) {
      const k = i + j;
      if (k >= 0 && k < x.length) acc += x[k] * taps[j + half];
    }
    out[i] = acc / sum;
  }
  return out;
}

function snrAt(ref, got, lag) {
  let sig = 0, err = 0;
  for (let i = 0; i < ref.length; ++i) {
    const j = i + lag;
    if (j < 0 || j >= got.length) continue;
    sig += ref[i] * ref[i];
    const e = ref[i] - got[j];
    err += e * e;
  }
  return err === 0 ? 99 : sig === 0 ? 0 : 10 * Math.log10(sig / err);
}

// { lag, low (dB, below 3 kHz), full (dB) }
function quality(ref, got, rate) {
  const a = lowpass(ref, rate), b = lowpass(got, rate);
  let best = { lag: 0, low: -1e9 };
  for (let lag = -32; lag <= 32; ++lag) {
    const v = snrAt(a, b, lag);
    if (v > best.low) best = { lag, low: v };
  }
  return { lag: best.lag, low: best.low, full: snrAt(ref, got, 0) };
}

function pack(o) {
  const [src, dst] = o.files;
  const b = new Uint8Array(fs.readFileSync(src));
  const streams = L.findStreams(b);
  if (!streams.length) {
    console.error(`${src}: no ADX stream SA2's decoder plays`);
    process.exit(1);
  }
  const opus = [];
  let worst = 99, sumLow = 0, checked = 0, lagged = 0, adxBytes = 0, opusBytes = 0;
  const t0 = Date.now();
  const every = streams.length > 50 ? 25 : 1;  // an archive's clips: every 25th checked (two FFmpeg runs each)
  streams.forEach((s, i) => {
    const pcm = L.decodeAdx(b, s);
    const up = pcm.map((ch) => L.resample(ch, s.rate, L.OPUS_RATE));
    const kbps = s.channels === 2 ? o.stereo : o.mono;
    const enc = encodeOpus(o, up, s.channels, kbps);
    opus.push(enc);
    let line = '';
    if (i % every === 0) {
      const back = decodeOgg(o, enc.ogg, s.channels).map((ch) => L.resample(ch, L.OPUS_RATE, s.rate));
      const qs = pcm.map((ch, c) => quality(ch, back[c], s.rate));
      const low = Math.min(...qs.map((q) => q.low));
      if (qs.some((q) => q.lag !== 0)) ++lagged;
      worst = Math.min(worst, low);
      sumLow += low;
      ++checked;
      line = `, lag ${qs.map((q) => q.lag).join('/')}, SNR below 3 kHz ${low.toFixed(1)} dB (full band ` +
             `${Math.min(...qs.map((q) => q.full)).toFixed(1)})`;
    }
    adxBytes += s.dataEnd - s.dataStart;
    opusBytes += enc.packets.reduce((a, p) => a + p.length, 0);
    if (!o.quiet && (streams.length < 20 || i % 200 === 0))
      console.log(`  stream ${i}: ${s.channels} ch ${s.rate} Hz, ${s.blocks} blocks (${(s.blocks * 32 / s.rate).toFixed(1)} s` +
                  `${s.ended ? '' : ', no end marker'}), Opus ${kbps} kbit/s ${enc.packets.length} packets, pre-skip ` +
                  `${enc.preSkip}${line}`);
  });
  const sumSnr = sumLow;
  const out = L.buildAdxo(b, streams, opus);
  fs.writeFileSync(dst + '.part', out);
  fs.renameSync(dst + '.part', dst);
  console.log(`${src}: ${streams.length} streams, ${(b.length / 1e6).toFixed(2)} MB -> ${(out.length / 1e6).toFixed(2)} MB ` +
              `(ADX blocks ${(adxBytes / 1e6).toFixed(2)} MB -> Opus ${(opusBytes / 1e6).toFixed(2)} MB); ${checked} checked: ` +
              `SNR below 3 kHz mean ${(sumSnr / Math.max(1, checked)).toFixed(1)} dB, worst ${worst.toFixed(1)} dB, ${lagged} ` +
              `not at lag 0; ${((Date.now() - t0) / 1000).toFixed(1)} s`);
}

function check(o) {
  const [src, dst] = o.files;
  const b = new Uint8Array(fs.readFileSync(src));
  const a = new Uint8Array(fs.readFileSync(dst));
  const streams = L.findStreams(b);
  const ids = streams.map((_, i) => 1000 + i);
  const { file } = L.readAdxo(a, ids);
  if (file.length !== b.length) throw new Error('the size differs');
  const inBlocks = new Uint8Array(b.length);
  for (const s of streams) inBlocks.fill(1, s.dataStart, s.dataEnd);
  let differ = 0, badTags = 0, blocks = 0;
  for (let i = 0; i < b.length; ++i) if (!inBlocks[i] && file[i] !== b[i]) ++differ;
  streams.forEach((s, k) => {
    for (let blk = 0; blk < s.blocks; ++blk)
      for (let c = 0; c < s.channels; ++c) {
        const t = L.parseTag(file, s.dataStart + (blk * s.channels + c) * L.BLOCK_BYTES);
        ++blocks;
        if (!t || t.id !== ids[k] || t.block !== blk || t.channel !== c) ++badTags;
      }
  });
  console.log(`${src}: bytes outside the blocks that differ: ${differ}; blocks ${blocks}, tags not as they should be: ${badTags}`);
}

const o = args();
if (o.check) check(o);
else pack(o);
