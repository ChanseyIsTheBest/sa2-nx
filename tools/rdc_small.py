# blur-nx (2026-11-11): every small texture of a RenderDoc capture (16x16 or less) with its value range at the end of
# the frame and the events that write it (run inside qrenderdoc; tools/rdc_run.ps1 sets the environment), to compare
# two renderers' small GPU results (luminance, ambient cubes) and the uploads they read.
#   RDC_IN = capture, RDC_OUT = folder for small.txt, RDC_MAX = largest width / height (default 16)
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
MAX = int(os.environ.get("RDC_MAX", "16"))
log = open(os.path.join(OUT, "small.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


WRITES = ("ColorTarget", "CopyDst", "ResolveDst", "CS_RWResource", "PS_RWResource", "Clear", "GenMips")

try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    for t in controller.GetTextures():
        if t.width > MAX or t.height > MAX:
            continue
        uses = controller.GetUsage(t.resourceId)
        writes = [u for u in uses if str(u.usage).split(".")[-1] in WRITES]
        reads = [u for u in uses if u not in writes and "Resource" in str(u.usage)]
        line = "tex %d %dx%d %s arr %d mips %d | writes %s | reads %d" % (
            int(t.resourceId), t.width, t.height, t.format.Name(), t.arraysize, t.mips,
            ",".join("%d:%s" % (u.eventId, str(u.usage).split(".")[-1]) for u in writes[:8]) or "-", len(reads))
        try:
            lo, hi = controller.GetMinMax(t.resourceId, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            line += " | min %s max %s" % (["%.5g" % v for v in lo.floatValue[:4]], ["%.5g" % v for v in hi.floatValue[:4]])
        except Exception as e:
            line += " | ? %s" % e
        if t.width * t.height <= 4:
            try:
                data = controller.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))
                line += " | bytes " + data[:16].hex()
            except Exception:
                pass
        say(line)
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
