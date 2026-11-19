# katamari-nx (2026-10-30): what a draw of a RenderDoc capture can read, and what is in it (run inside qrenderdoc;
# tools/rdc_run.ps1 sets the environment). For each event of RDC_EVENTS: every descriptor bound to the pixel shader
# (not only the ones RenderDoc sees used: Ryujinx's translated shaders show none), the depth target, and for each
# texture its size, format, sample count and value range at that event. Then every multisampled or depth texture of
# the capture with its range at the first event. Writes bindings.txt.
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
log = open(os.path.join(OUT, "bindings.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


def describe(controller, textures, rid):
    t = textures.get(rid)
    if t is None:
        return "resource %d (not a texture)" % int(rid)
    text = "tex %d %dx%d %s samples %d mips %d" % (int(rid), t.width, t.height, t.format.Name(), t.msSamp, t.mips)
    for sample in sorted({0, max(t.msSamp - 1, 0)}):
        try:
            lo, hi = controller.GetMinMax(rid, rd.Subresource(0, 0, sample), rd.CompType.Typeless)
            text += " | s%d min %s max %s" % (sample, ["%.6g" % v for v in lo.floatValue[:2]],
                                                ["%.6g" % v for v in hi.floatValue[:2]])
        except Exception as e:
            text += " | s%d ? %s" % (sample, e)
    return text


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError("open capture: %s" % status)
    textures = {t.resourceId: t for t in controller.GetTextures()}
    for ev in EVENTS:
        controller.SetFrameEvent(ev, True)
        st = controller.GetPipelineState()
        say("event", ev)
        try:
            dt = st.GetDepthTarget()
            if dt.resource != rd.ResourceId.Null():
                say("   depth target", describe(controller, textures, dt.resource))
        except Exception as e:
            say("   depth target ?", e)
        try:
            for i, d in enumerate(st.GetOutputTargets()):
                if d.resource != rd.ResourceId.Null():
                    say("   colour", i, describe(controller, textures, d.resource))
        except Exception as e:
            say("   colour ?", e)
        for stage in (rd.ShaderStage.Pixel,):
            try:
                seen = set()
                for used in st.GetReadOnlyResources(stage, False):
                    d = used.descriptor
                    if d.resource == rd.ResourceId.Null() or int(d.resource) in seen:
                        continue
                    seen.add(int(d.resource))
                    fmt = d.format.Name() if hasattr(d, "format") else "?"
                    say("   ps ro", describe(controller, textures, d.resource), "view", fmt)
                say("   ps ro:", len(seen), "resources")
            except Exception as e:
                say("   ps ro ?", e)
    if EVENTS:
        controller.SetFrameEvent(EVENTS[0], True)
    say("multisampled or depth textures at event", EVENTS[0] if EVENTS else "end")
    for rid, t in textures.items():
        name = t.format.Name()
        if t.msSamp > 1 or name.startswith("D") or "S8" in name:
            say("  ", describe(controller, textures, rid))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
