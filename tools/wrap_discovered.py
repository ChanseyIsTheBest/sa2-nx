#!/usr/bin/env python3
"""banjo-nx: wraps every raw shader the native renderer dumped (MASSEFFECT_SHADER_DISCOVERY folder: vs_/ps_<hash>.ucode)
into a register-linked container for the shader library (shaders/tools/wrap_raw_shader.py), into ../harvest_discovery.
Pixel shaders declare the exports their microcode really makes (colour 0..3, depth), found by walking the exec
clauses: wrap_raw_shader.py alone always declares colour 0 only.

Usage: python tools/wrap_discovered.py <discovery folder> <output folder> <vs template> <ps template>
"""
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def words(raw: bytes):
    return list(struct.unpack(">%dI" % (len(raw) // 4), raw))


def pixel_exports(w):
    """Bit mask of the PS exports: bits 0..3 colour targets, bit 4 depth (export register 61)."""
    mask = 0
    end = len(w) // 3
    pair = 0
    while pair < end and pair * 3 + 2 < len(w):
        cf = [w[pair * 3] | ((w[pair * 3 + 1] & 0xFFFF) << 32),
              (w[pair * 3 + 1] >> 16) | (w[pair * 3 + 2] << 16)]
        for c in cf:
            opcode = (c >> 44) & 0xF
            if not ((1 <= opcode <= 6) or opcode in (13, 14)):
                continue
            address, count, sequence = c & 0xFFF, (c >> 12) & 0x7, (c >> 16) & 0xFFF
            if address:
                end = min(end, address)
            for i in range(count):
                index = address + i
                if (sequence >> (i * 2)) & 1 or index * 3 + 2 >= len(w):
                    continue
                w0 = w[index * 3]
                if not (w0 >> 15) & 1:
                    continue  # not an export
                for dest, write in (((w0 & 0x3F), (w0 >> 16) & 0xF), (((w0 >> 8) & 0x3F), (w0 >> 20) & 0xF)):
                    if not write:
                        continue
                    if dest <= 3:
                        mask |= 1 << dest
                    elif dest == 61:
                        mask |= 0x10
        pair += 1
    return mask or 1


def main():
    discovery, output, vs_template, ps_template = map(pathlib.Path, sys.argv[1:5])
    output.mkdir(parents=True, exist_ok=True)
    tool = ROOT / "shaders" / "tools" / "wrap_raw_shader.py"
    made = 0
    for raw_path in sorted(discovery.glob("*.ucode")):
        stage = raw_path.name[:2]
        target = output / ("%s_d%s.bin" % (stage, raw_path.stem[3:]))
        template = vs_template if stage == "vs" else ps_template
        args = [sys.executable, str(tool), str(raw_path), str(template), str(target), "--stage", stage]
        exports = 0
        if stage == "ps":
            exports = pixel_exports(words(raw_path.read_bytes()))
            if exports & 0x10:
                args.append("--depth")
        subprocess.run(args, check=True)
        if stage == "ps":
            data = bytearray(target.read_bytes())
            shader = struct.unpack_from(">I", data, 24)[0]
            struct.pack_into(">I", data, shader + 28, exports)
            target.write_bytes(bytes(data))
        made += 1
    print("wrapped", made)


if __name__ == "__main__":
    main()
