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

## Log

- 2026-11-21: the inventory above; the interviews' menu; mwPlyGetCurFrm found.
- 2026-11-21: the movies' hooks (`app/src/sa2_hevc_movies.cpp`), rr6-nx's NAME.hevc reader (`sfd_hevc.cpp`, split
  so a pack entry can be one too: `OpenSfdHevcFrom`), the host folder's lookup (NAME.hevc before NAME) and the game
  pack's (`pack_device.cpp`: a NAME.hevc entry answers for NAME), nxvideo in the Switch build (`SA2_NXVIDEO`).
  PC (out/pc_runs/hevc_pc2): `op360us.sfd.hevc` opened (1,273 H.265 frames), every slice tagged and skipped, every
  frame given to the game carries its tag (the PC has no H.265 decoder: grey pictures, seen blended over the opening).
  `hevc_sfd_pack.py --check` on SA2's movies: e0401_m 5.5 -> 2.9 MB (PSNR mean 52.6 dB), op360us 3.0 -> 1.3 MB.
