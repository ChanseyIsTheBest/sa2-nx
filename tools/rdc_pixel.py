# banjo-nx (2026-10-18): the history of chosen pixels of a texture in a RenderDoc capture (run inside qrenderdoc):
#   qrenderdoc.exe --python tools/rdc_pixel.py
# Environment: RDC_IN (capture), RDC_OUT (folder for pixel.txt), RDC_TARGET (resource id of the texture, as the other
# scripts print it), RDC_PIXELS ("x,y;x,y"), RDC_EVENT (event to replay to, default the end of the frame).
# Every event that touched each pixel: its name, index count, test results, shader output and the value after it.
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
TARGET = int(os.environ.get("RDC_TARGET", "0"))
PIXELS = [tuple(int(c) for c in p.split(",")) for p in os.environ.get("RDC_PIXELS", "").split(";") if p.strip()]
EVENT = int(os.environ.get("RDC_EVENT", "0"))
log = open(os.path.join(OUT, "pixel.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    sfile = controller.GetStructuredFile()
    actions = flatten(controller.GetRootActions(), [])
    by_id = {a.eventId: a for a in actions}
    target = None
    for t in controller.GetTextures():
        if int(t.resourceId) == TARGET:
            target = t.resourceId
    if target is None:
        raise RuntimeError("no texture %d" % TARGET)
    controller.SetFrameEvent(EVENT or max(by_id), True)
    for (px, py) in PIXELS:
        say("pixel", px, py)
        hist = controller.PixelHistory(target, px, py, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        for m in hist:
            a = by_id.get(m.eventId)
            flags = [n for n in ("sampleMasked", "backfaceCulled", "depthClipped", "depthBoundsFailed", "viewClipped",
                                 "scissorClipped", "shaderDiscarded", "depthTestFailed", "stencilTestFailed")
                     if getattr(m, n, False)]
            say("  event", m.eventId, a.GetName(sfile) if a else "?", "indices", a.numIndices if a else "?",
                "prim", m.primitiveID, "flags", ",".join(flags) or "passed",
                "out %.3f %.3f %.3f %.3f" % tuple(m.shaderOut.col.floatValue[:4]),
                "after %.3f %.3f %.3f %.3f" % tuple(m.postMod.col.floatValue[:4]),
                "depth %.6f" % m.postMod.depth)
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
