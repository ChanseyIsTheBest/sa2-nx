# banjo-nx (2026-10-18): takes RenderDoc captures of a program started with renderdoccmd (Ryujinx running the NRO on
# its Vulkan back end, or renut.exe), at set times (run inside qrenderdoc):
#   qrenderdoc.exe --python tools/rdc_trigger.py
# Environment: RDC_OUT (folder for trigger.txt), RDC_TIMES (comma list of seconds after the connection, e.g.
# "200,230"), RDC_FRAMES (frames per capture, default 1), RDC_IDENT (target ident, default: the first one found).
# The captures go where renderdoccmd's --capture-file said; trigger.txt lists their paths.
import os
import shutil
import time
import traceback

import renderdoc as rd

OUT = os.environ.get("RDC_OUT", ".")
TIMES = [float(t) for t in os.environ.get("RDC_TIMES", "60").split(",") if t.strip()]
FRAMES = int(os.environ.get("RDC_FRAMES", "1"))
log = open(os.path.join(OUT, "trigger.txt"), "w")


def say(*parts):
    log.write(time.strftime("%H:%M:%S ") + " ".join(str(p) for p in parts) + "\n")
    log.flush()


def connect():
    ident = int(os.environ.get("RDC_IDENT", "0"))
    for _ in range(600):
        idents = [ident] if ident else []
        if not idents:
            i = rd.EnumerateRemoteTargets("", 0)
            while i:
                idents.append(i)
                i = rd.EnumerateRemoteTargets("", i)
        for i in idents:
            tc = rd.CreateTargetControl("", i, "banjo-nx rdc_trigger", True)
            if tc:
                say("connected to", i, tc.GetTarget(), "pid", tc.GetPID())
                return tc
        time.sleep(1)
    return None


try:
    tc = connect()
    if not tc:
        say("no target")
    else:
        start = time.time()
        pending = sorted(TIMES)
        taken = 0
        while pending or taken < len(TIMES):
            now = time.time() - start
            if pending and now >= pending[0]:
                say("trigger at", round(now, 1), "s")
                tc.TriggerCapture(FRAMES)
                pending.pop(0)
            msg = tc.ReceiveMessage(None)
            if msg.type == rd.TargetControlMessageType.NewCapture:
                taken += 1
                say("capture", msg.newCapture.captureId, msg.newCapture.path, "frame", msg.newCapture.frameNumber)
                # banjo-nx (2026-10-20): RenderDoc deletes a capture it handed to this connection when the connection
                # closes (run 75 lost all of them): a copy next to trigger.txt.
                try:
                    dest = os.path.join(OUT, "capture_%02d_frame%d.rdc" % (taken, msg.newCapture.frameNumber))
                    for _ in range(50):
                        if os.path.exists(msg.newCapture.path):
                            break
                        time.sleep(0.1)
                    shutil.copyfile(msg.newCapture.path, dest)
                    say("copied to", dest)
                except Exception as e:
                    say("copy failed", e)
            elif msg.type == rd.TargetControlMessageType.Disconnected:
                say("disconnected")
                break
            time.sleep(0.05)
        tc.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
