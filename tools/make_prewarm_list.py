#!/usr/bin/env python3
"""banjo-nx: turns a PC run's pipeline file into a first-boot prewarm list for the Switch.

cache/masseffect_native_pipelines.bin holds two parts (masseffect_native_draws.cpp, kMagicFilePipelines): the prewarm
list (pipeline keys, vertex layouts and shader fingerprints, all host-independent) and the driver's
vkGetPipelineCacheData blob. The PC blob is useless to NVK, so only the list is kept. With
masseffect_native_pipelines_prewarm = true the console compiles those pipelines on a lowest-priority thread from the
first boot (title screen, menus) instead of on the ring thread when a draw first needs them.

The list is only valid with the same shader library (records carry library numbers checked against fingerprints):
regenerate it whenever banjo_shaders.mesp changes.

Usage: python tools/make_prewarm_list.py <PC pipelines file> <output file> [--require-spec-bit=N]
  --require-spec-bit=N  keep only records whose key's specialization has bit N: after a shader package or renderer
                        change that sets a bit on every pipeline (e.g. 5, SPEC_CONSTANT_TEXCOORDS_UNSWAPPED with
                        banjo_gpu_texcoords_unswapped), the records made before it are keys no draw will ask for.
"""
import struct
import sys

MAGIC_FILE = 0x4350464E  # "NFPC"
VERSION_FILE = 1  # written: no cache, so no shader package tag (version 2 adds it after the sizes; both are read)
MAGIC_LIST = 0x4C50464E  # "NFPL"


def main():
    src, dst = sys.argv[1], sys.argv[2]
    require = [int(a.split('=', 1)[1]) for a in sys.argv[3:] if a.startswith('--require-spec-bit=')]
    data = open(src, "rb").read()
    magic, version, bytes_list, bytes_cache = struct.unpack_from("<IIQQ", data, 0)
    header = {1: 24, 2: 32}.get(version, 0)
    if magic != MAGIC_FILE or not header or header + bytes_list + bytes_cache != len(data):
        sys.exit("not a pipelines file of this version")
    lst = data[header:header + bytes_list]
    if len(lst) < 16:
        sys.exit("the file has no prewarm list")
    list_magic, list_version, record, count = struct.unpack_from("<IIII", lst, 0)
    if list_magic != MAGIC_LIST or 16 + record * count != len(lst):
        sys.exit("damaged prewarm list")
    # The same pipeline can appear twice: a record's key carries the library numbers of its shaders (the first 8
    # bytes of PipelineKey), which change when the library is rebuilt, while the prewarm thread finds the shaders by
    # their container fingerprints. One record per pipeline, at the position of its first use (the prewarm order) but
    # with the newest record's numbers (the current library's, which the ring's own lookups use).
    position = {}
    records = []
    for i in range(count):
        r = lst[16 + i * record:16 + (i + 1) * record]
        identity = r[8:]
        specialization = struct.unpack_from("<I", r, 20)[0]  # PipelineKey: vs, ps, entry (8), topology, specialization
        if any(not (specialization >> bit) & 1 for bit in require):
            continue
        if identity in position:
            records[position[identity]] = r
        else:
            position[identity] = len(records)
            records.append(r)
    lst = struct.pack("<IIII", list_magic, list_version, record, len(records)) + b"".join(records)
    with open(dst, "wb") as out:
        out.write(struct.pack("<IIQQ", MAGIC_FILE, VERSION_FILE, len(lst), 0))
        out.write(lst)
    print("%d pipelines (%d duplicates dropped; list version %d, %d-byte records); %d KB of PC driver cache dropped"
          % (len(records), count - len(records), list_version, record, bytes_cache >> 10))


if __name__ == "__main__":
    main()
