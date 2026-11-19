# blur-nx (2026-11-11): every use of chosen resources in a RenderDoc capture (run inside qrenderdoc, like
# rdc_bindings.py; tools/rdc_run.ps1 sets the environment), to find what writes a texture a draw reads.
#   RDC_IN = capture, RDC_OUT = folder for usage.txt, RDC_RESOURCES = "id,id,..." (as the other scripts print them)
# For each resource: its description, then each event that used it with the usage (render target, copy destination,
# resolve destination, shader read ...) and the action's name and its outputs.
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
IDS = [int(e) for e in os.environ.get("RDC_RESOURCES", "").split(",") if e.strip()]
log = open(os.path.join(OUT, "usage.txt"), "w")


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
    by_id = {}
    for r in controller.GetResources():
        by_id[int(r.resourceId)] = r
    actions = {a.eventId: a for a in flatten(controller.GetRootActions(), [])}
    sfile = controller.GetStructuredFile()
    for rid_int in IDS:
        rid = None
        for r in controller.GetResources():
            if int(r.resourceId) == rid_int:
                rid = r.resourceId
                break
        if rid is None:
            say("resource", rid_int, "not found")
            continue
        t = textures.get(rid_int)
        say("resource", rid_int, by_id[rid_int].name if rid_int in by_id else "",
            ("%dx%d %s mips %d samples %d" % (t.width, t.height, t.format.Name(), t.mips, t.msSamp)) if t else "")
        for u in controller.GetUsage(rid):
            ev = u.eventId
            a = actions.get(ev)
            name = a.GetName(sfile) if a else "?"
            outs = ""
            if a is not None:
                outs = " ".join(str(int(o)) for o in a.outputs if o != rd.ResourceId.Null())
                if a.copySource != rd.ResourceId.Null() or a.copyDestination != rd.ResourceId.Null():
                    outs += " copy %d -> %d" % (int(a.copySource), int(a.copyDestination))
            say("   event", ev, str(u.usage).split(".")[-1], "|", name[:80], "| outputs", outs)
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
