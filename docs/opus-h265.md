# The movies in H.265 and the audio in Opus (sa2-nx-ult, from 2026-11-21)

The user (2026-11-21): "can you please help sonic adventure 2 convert all its audio and videos over to opus and 265"
(through rr6-nx, whose handoff is `D:\ridge racer 6\rr6-nx\docs\opus-h265-handoff-sa2.md`), then in this session:
"Everything, audio too", and "yeah please rencode all the videos to h.265 please and all the audio to opus in a web
installer like culdcept". The re-encoding happens in the player's browser (game data is never distributed); the port
plays what it makes. Branch `ultimate` only (main is the shipped 1.0.6).

## What SA2 has (assets/game_root, measured 2026-11-21 from the files' headers)

| Files | Format | Size | Where it plays |
|---|---|---|---|
| `gd/interview.wmv` (5:56), `gd/interview_jp.wmv` (4:32) | ASF: WMV3 1920x1080, ~8-9.5 Mbit/s; WMA 9 Pro stereo 48 kHz | 658 MB | a submenu of the main menu (below); **never played in 1.0.6** |
| 11 cutscene movies (`gd/e0428_m.sfd`, `gd/e0609_m.sfd` (the Battle DLC's), `gd/event/e04xx_m.sfd`, `e05xx`, `e06xx`) | CRI Sofdec: MPEG-1 640x480 29.97 fps (one 30), ADX audio | 368 MB | the story's full-motion cutscenes; `e0401_m.sfd` in the attract sequence |
| 20 small clips (`gd/*.sfd`: monitors, a TV, `maria90sec`, `op360us`/`op360jp`, ...) | Sofdec: MPEG-1 320x240, no audio | 37 MB | on screens inside cutscenes and stages |
| 157 `gd/ADX/*.adx` | CRI ADX (encoding 3, 4 bits a sample): 134 stereo 32 kHz, 9 stereo 44.1 kHz, 13 mono 22.05 kHz, 1 header v4; 103 loop | 366 MB (3 h) | the music |
| `gd/event_adx.afs`, `gd/event_adx_e.afs` | CRI AFS archives of 2,727 mono 22.05 kHz ADX clips each | 146 MB (2.5 h each) | the cutscene voices (Japanese, English by the names) |
| 283 `*.csb` | CRI sound banks (`@UTF` tables) holding 7,467 ADX sounds (in AAX) | 50 MB | effects and voices in stages and the Chao areas |
| 52 `.mid` | MIDI | 66 KB | in the cutscene folders |

The game's CRI libraries (strings in `out/renut_image.bin`): ADXT/XBOX360 11.35, CRI MPV 2.081, CRI SFD 2.183, MWSFD
5.54 (RR6: MWSFD 3.71f, CRI MPV 1.976).

## The interviews

`sub_82121768` picks `interview_jp.wmv` (0x820E8C30) or `interview.wmv` (0x820E8C44) by a language byte and starts the
game's player (`sub_82121A00`, `sub_82120D68`; Microsoft's XMedia2 library at 0x8257xxxx). It is called from the main
menu's state machine (`sub_823D2838`, 24 states; the screen `sub_823D0C68`, sub-state 26 -> main state 22): the first
item of a six-item submenu whose other items are `advsng_5.adx` (shown after 5 hours of play time, 1,080,000 frames),
`advsng_1.adx`, two more screens and back. SA2's own code has no WMV decoder: XMedia2 loads the 360's "optional media
update" (`CodecPack02`, `CodecPackLoaderRoot`, `XamIsOptionalMediaUpdateInstalled`, `XamShowOptionalMediaUpdateRequiredUI`,
both stubs in the SDK), so 1.0.6 never shows them. Playing them is new work (a host decoder in the codec pack's place).

## The user's orders (2026-11-21)

- "rencode all the videos to h.265 please and all the audio to opus in a web installer like culdcept"
- "also want you to copy culdcept saga web installer for encoding audio and video please and compressing files with
  ZXC and L4Z"; "there's also ridge racer 6 web installer if its useful" (its SFD writer: `lib/sfdh.js`,
  `wasm/src/mpeg1`)

So the new package is culdcept-nx-installer's: loose game files in `game_root/`, the big ones packed as `NAME.lz4b`
(LZ4 or ZXC blocks, the base's and rr6-nx's reader: culdcept-nx's commit 90206e0a, taken here), the movies as
`NAME.hevc`, the sound as `NAME.adxo`, both left uncompressed (as Culdcept's). 1.0.6's `game_root.nxpack` stays
readable (the pack device knows `.hevc` and `.adxo` entries too).

## Plan

1. **The Sofdec movies (31)**: rr6-nx's path. NAME.hevc (`rex/filesystem/sfd_hevc.h`, "RXSFDH01") shows the game the
   SFD with each picture's slices zero but for a tag; hooks at CRI MPV's slice function and MWSFD's mwPlyGetCurFrm
   (SA2: `sub_828E06F8`, its frame record as RR6's: +0 the buffer, +12 / +16 the width and height, filled by
   `sub_828E01B0`) put the H.265 picture of that index in the frame. nxvideo (FFmpeg 7.1 + nvtegra, devkitPro's
   portlibs) on the Switch; its software decoder in Ryujinx.
2. **The audio**: SA2's ADX is decoded by CRI's ADX library in guest code. A tagged ADX stream (each 18-byte block a
   tag, the header and the end marker kept, so every file keeps its size and an AFS or SFD its offsets) beside the
   Opus, and a hook at the ADX block decoder that writes the Opus's PCM (resampled to the stream's rate) instead. The
   same hook covers the music, the AFS voices, the SFD movies' sound and (if worth it) the sound banks.
3. **The interviews**: H.265 + Opus, and their playback.
4. **The installer**: culdcept-nx-installer's way (WebCodecs H.265 and Opus encoders, FFmpeg decoders in WebAssembly,
   worker pools), with rr6-nx-installer's SFD writer (`lib/sfdh.js`, `wasm/src/mpeg1`).

## The movies: what SA2's code does (found 2026-11-21)

- `sub_82901C40(mpv, sj, out)`: CRI MPV's one slice: GetChunk into mpv+5288, the slice header (row, quantiser
  +5128, extra bytes), what it read given back (bit offset +5296), then the picture type's slice decoder mpv+5084;
  the picture is done when mpv+5204 (the last macroblock done) reaches mpv+5216.
- `sub_8296BA00` / `sub_8296C550` / `sub_8296BF68`: the I, P and B slice decoders (the PSFD00 section, table
  0x820BCCD0 by picture_coding_type, set by the picture header's parser `sub_828F7780`); reached only through
  mpv+5084. The slice's first byte holds the header's last 2 bits, so rr6-nx's tag (after its 0x08 quantiser byte)
  is at that byte + 1: `tools/hevc_sfd_pack.py` (rr6-nx's, unchanged) makes SA2's movies as they are.
- mpv+4992 / +4996 / +5000: the picture being decoded, Cb / Cr / Y (+4976..+4984 the one before); the layout is
  rr6-nx's SofdecPlanes (320x240: Y pitch 384, Cb 92,160 bytes on, pitch 256; Cr 30,720 after).
- `sub_828E06F8(ply, frm)`: mwPlyGetCurFrm, reached only through a pointer; frm +0 the buffer (the Y plane),
  +12 / +16 width and height.
- The attract sequence opens `gd/op360us.sfd` (320x240, the overlay on the real-time opening) about 43 s in.

## The audio: what SA2's code does (found 2026-11-21)

- ADXT 11.35's one decode step: `sub_828C2948(obj)`: obj +12 the state (1 / 2 pending, 3 done), +16 the blocks
  decoded, +20 the channels, +24 the input (18-byte blocks, stereo interleaved L R), +28 the blocks (both channels'),
  +32 / +36 the outputs (16-bit PCM, 32 samples a block), +40 / +44 the channels' two-sample histories, +48 / +50
  the prediction coefficients, +52..+56 the key's state (CRI's scale XOR, off in SA2's files: flags byte 0).
  Mono: `sub_828CCD28(in, n, out, &hist, c1, c2, &key, ...)`; stereo: `sub_828CD3E0` -> `sub_828CCE80` /
  `sub_828CD168`. A block whose scale has its high bit set ends the stream (the decoders return early).
- All 157 music files: version 3 (one v4), flags 0 (no encryption).
- The decoders' formula (from their PPC): scale = ((field ^ key) & 0x1FFF) + 1 (key 0 here), sample = nibble * scale +
  ((c1 * s1 + c2 * s2) >> 12), clamped; high nibble first. FFmpeg's adpcm_adx uses the field without the + 1: the two
  decodes agree to 49.6 dB on title.adx; `tools/adx_opus_lib.mjs` decodes as the game does.

**The voices are not ADX (2026-11-21):** the clips of `gd/event_adx.afs` and `gd/event_adx_e.afs` have the ADX
signature but encoding 0x10: CRI **AHX** (an MPEG-1 Layer II variant; the game's "AHX Decoder" strings), about
63 kbit/s mono 22.05 kHz. The first inventory (and rr6-nx's handoff) read only the signature. The ADX hook does not see
them; Opus for them (about half their 146 MB at 32 kbit/s) needs a hook on the AHX decoder: a separate step.

## The audio: NAME.adxo (2026-11-21)

`sdk/include/rex/filesystem/adx_opus.h` has the format: rr6-nx's segment layout (stored bytes, zeros, and runs of ADX
blocks whose bytes are made as tags), a stream table, each stream's Opus packets (20 ms, 48 kHz, its channels). The
console makes the game's file with every block a tag (stream id, block index, channel); the hook on the decode step
(`app/src/sa2_adx_opus.cpp`, `sub_828C2948`) writes each tagged block's 32 samples from `AdxOpusBlockSamples` (the
SDK: libopus, the 48 kHz window kept a second back, a seek decodes from 4 packets before, a zero-phase Kaiser sinc
down to the stream's rate) and ends the step at the first block that is not one (an end marker), as the game's does.

`tools/adx_opus_pack.mjs` (with `tools/adx_opus_lib.mjs`, the part the installer shares) makes them on a PC with
FFmpeg's libopus: music 128 kbit/s stereo, mono 40. Checks: every byte outside the blocks as it was, every tag right;
the Opus decoded back and resampled is at lag 0 against the game's decode (title.adx: SNR below 3 kHz 19.9 dB; Opus
is perceptual: its full-band waveform SNR is 15-20 dB at any rate, so that number is not the measure). title.adx
0.75 -> 0.33 MB. The resampler's round trip alone (32 -> 48 -> 32 kHz): lag 0, 38 dB (the band above 15.5 kHz).

## Log

- 2026-11-21: the inventory above; the interviews' menu; mwPlyGetCurFrm found.
- 2026-11-21: the movies' hooks (`app/src/sa2_hevc_movies.cpp`), rr6-nx's NAME.hevc reader (`sfd_hevc.cpp`, split
  so a pack entry can be one too: `OpenSfdHevcFrom`), the host folder's lookup (NAME.hevc before NAME) and the game
  pack's (`pack_device.cpp`: a NAME.hevc entry answers for NAME), nxvideo in the Switch build (`SA2_NXVIDEO`).
  PC (out/pc_runs/hevc_pc2): `op360us.sfd.hevc` opened (1,273 H.265 frames), every slice tagged and skipped, every
  frame given to the game carries its tag (the PC has no H.265 decoder: grey pictures, seen blended over the opening).
  `hevc_sfd_pack.py --check` on SA2's movies: e0401_m 5.5 -> 2.9 MB (PSNR mean 52.6 dB), op360us 3.0 -> 1.3 MB.
- 2026-11-21: all 31 movies convert (`hevc_sfd_pack.py`: a picture's display index is now its rank among the display
  times: e015bmv, e015mva and gerald35 end on a GOP whose temporal references skip numbers; the installer's
  `lib/sfdh.js` the same). 15 movies have B pictures (all eleven 640x480 cutscenes): the hook's Y plane (mpv+5000)
  is confirmed for I and P pictures only; B is the next check. With nvenc: 425 -> 185 MB.
- 2026-11-21 (the user: "we want to be able to run ZXC compressed H265 videos"): NAME.hevc.lz4b (rr6-nx's of the same
  day): the movie's .hevc packed as ZXC blocks; `sfd_hevc.cpp` reads it through OpenLz4Blocks (both handles), the
  host folder's lookup takes it for NAME. The ZXC reader is culdcept-nx's commit 90206e0a (rr6-nx's reader, LZ4
  1.10.0, ZXC 0.15.0), applied here. `out/zxcb_pack.exe` (tools/zxcb_pack.cpp, built with devkitPro's clang64; run
  it with that folder on PATH): the 31 movies 184.6 -> 157.9 MB (14.4 % smaller). PC (out/pc_runs/zxchevc_pc1):
  op360us.sfd.hevc.lz4b opened (1,273 frames), tagged frames shown.
- 2026-11-21: the audio: `NAME.adxo` (format, reader `adx_opus.cpp`, hook `sa2_adx_opus.cpp`, tools). PC
  (out/pc_runs/hevc_adxo_pc1, 63 music files as .adxo): btl_opng, btl_hb, btl_pc opened, every decode step taken
  from the .adxo (0.005-0.1 ms a step), the game runs on; the PC has no libopus, so silence there (the Switch links
  devkitPro's). All music converted so far at lag 0.
- 2026-11-21, the emulator (out/emu_runs/emu_av2, 16 min of the attract with the 31 movies as .hevc.lz4b and 125
  music files as .adxo; nxvideo's software decoder): e0401_m (the logo movie at every start of the attract loop,
  640x480 with B pictures) opened from its ZXC file three times and shown from H.265; btl_hb, m_space, btl_pc, c_wall
  played from Opus (10,186 packets, loops seeking, 0.7-4 ms a decode step in the emulator, no step left to ADX). No
  error. **The B pictures:** +4992..+5000 is a B picture's backward reference, its own planes are +5008..+5016
  (the log's B slices), so the hook tagged the reference (wrong index shown) and left the B picture untagged; fixed
  (kPictureY 5016), with a [hevc] warning for a frame shown without a tag while a movie plays.
- 2026-11-22: out/emu_runs/emu_bfix: the logo movie with the fix, 0 frames shown without a tag.
- 2026-11-22 (the user: "just LZ4 compress everything besides the ZXC compress the H.265 videos"): measured with the
  installer's own packers: the game files the installer packs, 932 MB -> 502 MB with LZ4 HC (the .pak archives 363.5
  -> 60 MB), 486 MB with ZXC; so LZ4 for them, ZXC for the H.265 movies only.
- 2026-11-22, **the installer end to end** (sa2-nx-ult-installer, branch `sa2`, 7822ee5): its release is the
  release-shaders NRO of 120d675d (`app/out/switch-relshaders`, no PGO / LTO). Node (test/full_node.mjs): 47 s, 103
  shaders (97 the port's GLSL byte for byte, 6 with other temporaries' numbers, as 1.0.6's installer). The browser
  (test/browser.html in the pane's Chromium 156, the PC's H.265 encoder): 517 s, the zip 1,222 MB from the game's
  2,400 MB (the 33 movies 1,063 -> 399 MB, the music 365 -> 164 MB, the rest 932 -> 502 MB); check_zip: every entry
  and game file right. The emulator on that zip as a player gets it (out/emu_runs/emu_installer1: the release NRO, the
  page's deko3d_glsl.bin, its game_root): the GLSL package's 103 shaders loaded, title.adx played from the page's
  Opus, the logo movie shown from the page's H.265 (ZXC) to picture 329 of 335, 0 untagged frames, 0 errors.
- 2026-11-22 (the user: "an option in the file not to include the wmv interview videos to save space"): installer
  4dea4ad, game.json `"leave_out"` (a box on the page, ticked by default: the port does not play the interviews). Note
  the units: the zip above is 1,222,462,113 bytes, 1,166 MB in the 2,400 MB's units (MiB). Browser test with
  `?leave_out=interviews`: 970,480,872 bytes (926 MB) in 260 s (517 s with them); check_zip OK. Not checked: what the
  game does when the interviews' menu item is chosen with the files absent (1.0.6 never played them either).
