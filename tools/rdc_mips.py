# banjo-nx: value range of every mip level of chosen textures of a RenderDoc capture (run inside qrenderdoc, like
# rdc_passes.py).
#   RDC_IN = capture, RDC_OUT = folder for mips.txt, RDC_TEXTURES = "id;id;..." (resource numbers as rdc_draws.py
#   prints them), RDC_EVENT = event to look at (default: the last one)
# For each texture: size, format, level count, and per level the minimum and maximum of each channel. A level that
# is all zero was never filled (a streamed texture whose large levels have not arrived).
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
IDS = [int(v) for v in os.environ.get("RDC_TEXTURES", "").split(";") if v]
EVENT = int(os.environ.get("RDC_EVENT", "0"))
log = open(os.path.join(OUT, "mips.txt"), "w")


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
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    actions = flatten(controller.GetRootActions(), [])
    controller.SetFrameEvent(EVENT if EVENT else actions[-1].eventId, True)
    for number in IDS:
        t = textures.get(number)
        if not t:
            say(number, "no such texture")
            continue
        say(number, "%dx%dx%d" % (t.width, t.height, t.depth), t.format.Name(), "levels", t.mips, "layers",
            t.arraysize, str(t.type).split(".")[-1])
        for mip in range(t.mips):
            sub = rd.Subresource(mip, 0, 0)
            lo, hi = controller.GetMinMax(t.resourceId, sub, rd.CompType.Typeless)
            say("   level %d: min (%.3f %.3f %.3f %.3f) max (%.3f %.3f %.3f %.3f)" % (
                mip, lo.floatValue[0], lo.floatValue[1], lo.floatValue[2], lo.floatValue[3],
                hi.floatValue[0], hi.floatValue[1], hi.floatValue[2], hi.floatValue[3]))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
