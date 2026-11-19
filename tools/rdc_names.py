# blur-nx (2026-11-11): the names RenderDoc holds for chosen resources (run inside qrenderdoc; tools/rdc_run.ps1 sets the
# environment): Renut names its shaders and pipelines after the guest shader's hash.
#   RDC_IN = capture, RDC_OUT = folder for names.txt, RDC_RESOURCES = "id,id,..."
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
IDS = {int(e) for e in os.environ.get("RDC_RESOURCES", "").split(",") if e.strip()}
log = open(os.path.join(OUT, "names.txt"), "w")
try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    for r in controller.GetResources():
        if int(r.resourceId) in IDS:
            log.write("%d %s %s\n" % (int(r.resourceId), str(r.type), r.name))
    controller.Shutdown()
    cap.Shutdown()
    log.write("done\n")
except Exception:
    log.write("ERROR " + traceback.format_exc())
log.close()
os._exit(0)
