# banjo-nx (2026-10-18): buffer contents at chosen events of a RenderDoc capture (run inside qrenderdoc):
#   qrenderdoc.exe --python tools/rdc_buffer.py
# Environment: RDC_IN (capture), RDC_OUT (folder for buffer.txt), RDC_DUMPS ("event:resource:offset:bytes;..."; offset
# and bytes decimal or 0x hex). Each dump: the words as hex, as little-endian floats and as byte-swapped floats.
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
DUMPS = [d for d in os.environ.get("RDC_DUMPS", "").split(";") if d.strip()]
log = open(os.path.join(OUT, "buffer.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    buffers = {int(b.resourceId): b for b in controller.GetBuffers()}
    for d in DUMPS:
        eid, rid, off, size = [int(x, 0) for x in d.split(":")]
        b = buffers.get(rid)
        if not b:
            say("buffer", rid, "not found")
            continue
        controller.SetFrameEvent(eid, True)
        data = controller.GetBufferData(b.resourceId, off, size)
        say("event", eid, "buffer", rid, "(%d bytes)" % b.length, "offset", hex(off), "bytes", len(data))
        for row in range(0, len(data) - len(data) % 4, 32):
            chunk = data[row:row + 32]
            n = len(chunk) // 4
            words = struct.unpack_from("<%dI" % n, chunk)
            le = struct.unpack_from("<%df" % n, chunk)
            be = struct.unpack_from(">%df" % n, chunk)
            say("  +%04x" % row, " ".join("%08X" % w for w in words))
            say("        le", " ".join("%.4g" % f for f in le))
            say("        be", " ".join("%.4g" % f for f in be))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
