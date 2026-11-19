# banjo-nx (2026-10-20): saves the colour target of the last draw into the biggest target of a RenderDoc capture (the
# frame as it looked), to see which scene a capture caught (run inside qrenderdoc, like rdc_passes.py).
#   RDC_IN = capture, RDC_OUT = PNG path. Writes <RDC_OUT>.txt with the event id and target.
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
log = open(OUT + ".txt", "w")


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
    textures = {t.resourceId: t for t in controller.GetTextures()}
    best = None
    for a in flatten(controller.GetRootActions(), []):
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        o = a.outputs[0] if len(a.outputs) else rd.ResourceId.Null()
        t = textures.get(o)
        if t is None:
            continue
        area = t.width * t.height
        if best is None or area >= best[0]:
            best = (area, a.eventId, o)
    if best:
        controller.SetFrameEvent(best[1], True)
        s = rd.TextureSave()
        s.resourceId = best[2]
        s.destType = rd.FileType.PNG
        s.alpha = rd.AlphaMapping.Discard
        controller.SaveTexture(s, OUT)
        t = textures[best[2]]
        log.write("event %d target %d %dx%d %s\n" % (best[1], int(best[2]), t.width, t.height, t.format.Name()))
    controller.Shutdown()
    cap.Shutdown()
    log.write("done\n")
except Exception:
    log.write("ERROR " + traceback.format_exc())
log.close()
os._exit(0)
