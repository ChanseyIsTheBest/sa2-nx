# blur-nx (2026-11-11): the constant buffers chosen draws read, as floats (run inside qrenderdoc; tools/rdc_run.ps1 sets
# the environment), to compare two renderers' shader constants at the same draw (Renut's float constants and the SDK's
# emulated GPU's are both the game's c0..c255, each in its own buffer layout).
#   RDC_IN = capture, RDC_OUT = folder for consts.txt, RDC_EVENTS = "e1,e2,...", RDC_FLOATS = floats per buffer (1024)
# For each event, the vertex and pixel stages: every bound constant buffer's resource, offset and size, then its floats
# four to a line with the line's index (non-zero lines only).
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
FLOATS = int(os.environ.get("RDC_FLOATS", "1024"))
log = open(os.path.join(OUT, "consts.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    for ev in EVENTS:
        controller.SetFrameEvent(ev, True)
        st = controller.GetPipelineState()
        say("event", ev)
        for stage, label in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Pixel, "ps")):
            try:
                blocks = st.GetConstantBlocks(stage, False)
            except Exception as e:
                say("  ", label, "blocks ?", e)
                continue
            for i, used in enumerate(blocks):
                d = used.descriptor
                if d.resource == rd.ResourceId.Null():
                    continue
                size = d.byteSize if d.byteSize else 16 * 256
                size = min(size, FLOATS * 4)
                try:
                    raw = controller.GetBufferData(d.resource, d.byteOffset, size)
                except Exception as e:
                    say("  ", label, "cb", i, "?", e)
                    continue
                n = len(raw) // 4
                vals = struct.unpack_from("<%df" % n, raw)
                say("  %s cb %d resource %d offset %d size %d" % (label, i, int(d.resource), d.byteOffset, len(raw)))
                for j in range(0, n - 3, 4):
                    v = vals[j:j + 4]
                    if any(abs(x) > 1e-12 for x in v):
                        say("    [%3d] %12.6g %12.6g %12.6g %12.6g" % ((j // 4,) + tuple(v)))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
