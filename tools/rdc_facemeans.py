# blur-nx (2026-11-11): the mean of each face (array slice) of the six-slice textures a capture holds at a given size
# (run inside qrenderdoc; tools/rdc_run.ps1 sets the environment), to see which faces of a cube map were written.
#   RDC_IN = capture, RDC_OUT = folder for facemeans.txt, RDC_SIZE = face size (256), RDC_EVENT = event to replay to
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
SIZE = int(os.environ.get("RDC_SIZE", "256"))
EVENT = int(os.environ.get("RDC_EVENT", "0"))
log = open(os.path.join(OUT, "facemeans.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    if EVENT:
        controller.SetFrameEvent(EVENT, True)
    for t in controller.GetTextures():
        if t.width != SIZE or t.height != SIZE or t.arraysize < 6:
            continue
        if t.format.compByteWidth != 1 or t.format.compCount != 4:
            say("tex %d %s arr %d (not 8-bit four-channel)" % (int(t.resourceId), t.format.Name(), t.arraysize))
            continue
        parts = []
        for face in range(min(t.arraysize, 6)):
            data = controller.GetTextureData(t.resourceId, rd.Subresource(0, face, 0))
            n = SIZE * SIZE
            step = 7  # every 7th texel: enough for a mean
            sums = [0, 0, 0]
            count = 0
            for i in range(0, n, step):
                sums[0] += data[i * 4]
                sums[1] += data[i * 4 + 1]
                sums[2] += data[i * 4 + 2]
                count += 1
            parts.append("%.0f/%.0f/%.0f" % (sums[0] / count, sums[1] / count, sums[2] / count))
        say("tex %d %s arr %d faces %s" % (int(t.resourceId), t.format.Name(), t.arraysize, "  ".join(parts)))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
