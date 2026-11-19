# banjo-nx (2026-10-18): per-draw detail of a RenderDoc capture's event range (run inside qrenderdoc):
#   qrenderdoc.exe --python tools/rdc_draw_detail.py
# Environment: RDC_IN (capture), RDC_OUT (folder), RDC_FIRST / RDC_LAST (event ids). For each draw in the range:
# event, name, vertex count, viewport, scissor, colour outputs, blend of target 0, and the pixel shader's bound textures
# (resource id, size, format); every texture read is saved once (PNG, floats stretched over their range), and each
# draw's output after it. Writes draws.txt.
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
FIRST = int(os.environ.get("RDC_FIRST", "0"))
LAST = int(os.environ.get("RDC_LAST", "1000000000"))
log = open(os.path.join(OUT, "draws.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def save_texture(controller, textures, rid, path):
    t = textures.get(rid)
    if not t:
        return "?"
    save = rd.TextureSave()
    save.resourceId = rid
    save.destType = rd.FileType.PNG
    save.alpha = rd.AlphaMapping.Discard
    save.mip = 0
    save.slice.sliceIndex = 0
    if t.msSamp > 1:
        save.sample.mapToArray = False
        save.sample.sampleIndex = 0
    extra = ""
    if t.format.compType == rd.CompType.Float and t.format.compCount <= 2:
        lo, hi = controller.GetMinMax(rid, rd.Subresource(0, 0, 0), rd.CompType.Float)
        save.comp.blackPoint = lo.floatValue[0]
        save.comp.whitePoint = hi.floatValue[0] if hi.floatValue[0] > lo.floatValue[0] else lo.floatValue[0] + 1e-6
        extra = " range %.6f..%.6f" % (lo.floatValue[0], hi.floatValue[0])
    ok = controller.SaveTexture(save, path) == rd.ResultCode.Succeeded
    return ("saved" if ok else "not saved") + extra


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError("open capture: %s" % status)
    sfile = controller.GetStructuredFile()
    textures = {t.resourceId: t for t in controller.GetTextures()}
    names = {r.resourceId: r.name for r in controller.GetResources()}
    actions = flatten(controller.GetRootActions(), [])
    saved_inputs = set()
    for a in actions:
        if a.eventId < FIRST or a.eventId > LAST:
            continue
        if not (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Copy | rd.ActionFlags.Resolve | rd.ActionFlags.Clear)):
            continue
        controller.SetFrameEvent(a.eventId, True)
        if not (a.flags & rd.ActionFlags.Drawcall):
            say("event", a.eventId, a.GetName(sfile), "src", int(a.copySource), "dst", int(a.copyDestination))
            continue
        st = controller.GetPipelineState()
        vp = st.GetViewport(0)
        sc = st.GetScissor(0)
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        out_desc = []
        for o in outs:
            t = textures.get(o)
            out_desc.append("%d(%dx%d %s)" % (int(o), t.width, t.height, t.format.Name()) if t else str(int(o)))
        blends = st.GetColorBlends()
        b = blends[0] if len(blends) else None
        bdesc = ("blend %s %s/%s %s/%s mask %x" % (b.enabled, b.colorBlend.source, b.colorBlend.destination,
                                                     b.alphaBlend.source, b.alphaBlend.destination, b.writeMask)) if b else ""
        say("event", a.eventId, a.GetName(sfile), "verts", a.numIndices, "inst", a.numInstances,
            "viewport %.1f,%.1f %.1fx%.1f" % (vp.x, vp.y, vp.width, vp.height),
            "scissor %d,%d %dx%d" % (sc.x, sc.y, sc.width, sc.height), "out", " ".join(out_desc), bdesc)
        say("   vs", int(st.GetShader(rd.ShaderStage.Vertex)), "ps", int(st.GetShader(rd.ShaderStage.Pixel)),
            "pipeline", names.get(st.GetGraphicsPipelineObject(), "?"))
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            try:
                ro = st.GetReadOnlyResources(stage)
            except Exception:
                ro = []
            for used in ro:
                desc = used.descriptor if hasattr(used, "descriptor") else None
                rid = desc.resource if desc is not None else rd.ResourceId.Null()
                if rid == rd.ResourceId.Null():
                    continue
                t = textures.get(rid)
                if not t:
                    continue
                info = "%dx%d %s msaa%d" % (t.width, t.height, t.format.Name(), t.msSamp)
                note = ""
                if (rid, a.eventId) not in saved_inputs and rid not in [x[0] for x in saved_inputs]:
                    note = save_texture(controller, textures, rid, os.path.join(OUT, "in_e%d_%d.png" % (a.eventId, int(rid))))
                    saved_inputs.add((rid, a.eventId))
                say("   ", "vs" if stage == rd.ShaderStage.Vertex else "ps", "tex", int(rid), info, note)
        for o in outs:
            save_texture(controller, textures, o, os.path.join(OUT, "out_e%d_%d.png" % (a.eventId, int(o))))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
