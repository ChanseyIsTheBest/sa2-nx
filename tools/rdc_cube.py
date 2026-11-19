# blur-nx (2026-11-11): the small cube maps a RenderDoc capture uploads in its frame (run inside qrenderdoc; tools/rdc_run.ps1
# sets the environment): every 8-bit four-channel texture of six slices, RDC_SIZE square (default 8), written in the frame,
# with each face's texels in hex (memory order) and its byte means. For Blur's ambient cube: written by the CPU, read by the
# car and scenery shaders.
#   RDC_IN = capture, RDC_OUT = folder for cube.txt, RDC_SIZE = face size
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
SIZE = int(os.environ.get("RDC_SIZE", "8"))
log = open(os.path.join(OUT, "cube.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    for t in controller.GetTextures():
        if t.width != SIZE or t.height != SIZE or t.arraysize != 6:
            continue
        if t.format.compByteWidth != 1 or t.format.compCount != 4:
            continue
        uses = controller.GetUsage(t.resourceId)
        writes = [u.eventId for u in uses if "CopyDst" in str(u.usage) or "ColorTarget" in str(u.usage)]
        if not writes:
            continue
        say("cube %d %s written at %s" % (int(t.resourceId), t.format.Name(), writes[:6]))
        for face in range(6):
            data = controller.GetTextureData(t.resourceId, rd.Subresource(0, face, 0))
            n = SIZE * SIZE
            means = [sum(data[i * 4 + c] for i in range(n)) / n for c in range(4)]
            say("  face %d means %s" % (face, " ".join("%.1f" % m for m in means)))
            for row in range(SIZE):
                say("    " + " ".join("%02x%02x%02x%02x" % tuple(data[(row * SIZE + x) * 4:(row * SIZE + x) * 4 + 4])
                                      for x in range(SIZE)))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
