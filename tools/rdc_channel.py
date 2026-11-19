# banjo-nx (2026-10-18): one channel of a texture at an event of a RenderDoc capture, as a PNG (run inside qrenderdoc):
#   qrenderdoc.exe --python tools/rdc_channel.py
# Environment: RDC_IN, RDC_OUT, RDC_TARGET (resource id), RDC_EVENT, RDC_CHANNEL (0-3), RDC_NAME (file name).
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
TARGET = int(os.environ.get("RDC_TARGET", "0"))
EVENT = int(os.environ.get("RDC_EVENT", "0"))
CHANNEL = int(os.environ.get("RDC_CHANNEL", "3"))
NAME = os.environ.get("RDC_NAME", "channel.png")
log = open(os.path.join(OUT, "channel.txt"), "a")
try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    tex = [t for t in controller.GetTextures() if int(t.resourceId) == TARGET][0]
    controller.SetFrameEvent(EVENT, True)
    save = rd.TextureSave()
    save.resourceId = tex.resourceId
    save.destType = rd.FileType.PNG
    save.mip = 0
    save.slice.sliceIndex = 0
    save.channelExtract = CHANNEL
    ok = controller.SaveTexture(save, os.path.join(OUT, NAME))
    log.write("%s event %d texture %d channel %d: %s\n" % (NAME, EVENT, TARGET, CHANNEL, ok))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    log.write("ERROR " + traceback.format_exc())
log.close()
os._exit(0)
