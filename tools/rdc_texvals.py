# blur-nx (2026-11-11): the contents of the textures chosen draws read, to compare two renderers' inputs at the same
# draw (run inside qrenderdoc, like rdc_bindings.py; tools/rdc_run.ps1 sets the environment).
#   RDC_IN = capture, RDC_OUT = folder for texvals.txt, RDC_EVENTS = "e1,e2,..."
# For every texture bound to the vertex and pixel shaders at each event: size, format, the view's swizzle, and for
# 8-bit four-channel textures the mean of each byte of the texel (memory order) over level 0; textures of 64 texels
# or fewer are listed texel by texel. Other formats get RenderDoc's min / max.
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
log = open(os.path.join(OUT, "texvals.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


def swizzle(d):
    try:
        s = d.swizzle
        return "".join(str(c).split(".")[-1][0] for c in (s.red, s.green, s.blue, s.alpha))
    except Exception:
        return "?"


def contents(controller, t, rid):
    name = t.format.Name()
    if t.format.compByteWidth == 1 and t.format.compCount == 4 and t.format.type in (
            rd.ResourceFormatType.Regular,):
        data = controller.GetTextureData(rid, rd.Subresource(0, 0, 0))
        n = t.width * t.height
        if len(data) < n * 4:
            return "short data %d" % len(data)
        sums = [0, 0, 0, 0]
        for i in range(n):
            for c in range(4):
                sums[c] += data[i * 4 + c]
        text = "byte means %s" % " ".join("%.1f" % (s / n) for s in sums)
        if n <= 64:
            text += "\n      texels " + " ".join("%02x%02x%02x%02x" % tuple(data[i * 4:i * 4 + 4]) for i in range(n))
        return text
    try:
        lo, hi = controller.GetMinMax(rid, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        return "min %s max %s" % (["%.4g" % v for v in lo.floatValue[:4]], ["%.4g" % v for v in hi.floatValue[:4]])
    except Exception as e:
        return "? %s" % e


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    textures = {t.resourceId: t for t in controller.GetTextures()}
    for ev in EVENTS:
        controller.SetFrameEvent(ev, True)
        st = controller.GetPipelineState()
        say("event", ev)
        for stage, label in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Pixel, "ps")):
            seen = set()
            try:
                for used in st.GetReadOnlyResources(stage, True):
                    d = used.descriptor
                    if d.resource == rd.ResourceId.Null() or int(d.resource) in seen:
                        continue
                    seen.add(int(d.resource))
                    t = textures.get(d.resource)
                    if t is None:
                        continue
                    say("   %s tex %d %dx%d %s swz %s" % (label, int(d.resource), t.width, t.height, t.format.Name(),
                                                         swizzle(d)))
                    say("      ", contents(controller, t, d.resource))
            except Exception as e:
                say("   %s ?" % label, e)
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
