# banjo-nx (2026-10-18): a depth-stencil texture's stencil (and depth) at an event of a RenderDoc capture, as PNGs and
# a histogram (run inside qrenderdoc):  qrenderdoc.exe --python tools/rdc_stencil.py
# Environment: RDC_IN, RDC_OUT, RDC_TARGET (resource id), RDC_EVENT, RDC_PIXELS ("x,y;x,y": values printed).
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
TARGET = int(os.environ.get("RDC_TARGET", "0"))
EVENT = int(os.environ.get("RDC_EVENT", "0"))
PIXELS = [tuple(int(c) for c in p.split(",")) for p in os.environ.get("RDC_PIXELS", "").split(";") if p.strip()]
log = open(os.path.join(OUT, "stencil.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    tex = [t for t in controller.GetTextures() if int(t.resourceId) == TARGET][0]
    controller.SetFrameEvent(EVENT, True)
    say("texture", TARGET, tex.width, tex.height, tex.format.Name())
    for channel, name in ((1, "stencil"), (0, "depth")):
        save = rd.TextureSave()
        save.resourceId = tex.resourceId
        save.destType = rd.FileType.PNG
        save.mip = 0
        save.slice.sliceIndex = 0
        save.channelExtract = channel
        if channel == 1:
            save.comp.blackPoint = 0.0
            save.comp.whitePoint = 1.0
        ok = controller.SaveTexture(save, os.path.join(OUT, "%s_e%d.png" % (name, EVENT)))
        say(name, "saved" if ok == rd.ResultCode.Succeeded else ok)
    for (x, y) in PIXELS:
        v = controller.PickPixel(tex.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        say("pixel", x, y, "float", ["%.6f" % f for f in v.floatValue[:4]], "uint", list(v.uintValue[:4]))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
