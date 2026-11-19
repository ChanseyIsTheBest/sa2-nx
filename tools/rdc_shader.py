# blur-nx (2026-11-11): the disassembly of chosen draws' shaders (run inside qrenderdoc; tools/rdc_run.ps1 sets the
# environment), to see which constant registers a translated shader reads.
#   RDC_IN = capture, RDC_OUT = folder for shader_<event>_<stage>.txt, RDC_EVENTS = "e1,e2,...", RDC_STAGES = "vs,ps"
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
STAGES = [s.strip() for s in os.environ.get("RDC_STAGES", "ps").split(",") if s.strip()]
log = open(os.path.join(OUT, "shader.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    stage_of = {"vs": rd.ShaderStage.Vertex, "ps": rd.ShaderStage.Pixel}
    for ev in EVENTS:
        controller.SetFrameEvent(ev, True)
        st = controller.GetPipelineState()
        for name in STAGES:
            stage = stage_of[name]
            refl = st.GetShaderReflection(stage)
            if refl is None:
                say("event", ev, name, "no shader")
                continue
            pipe = st.GetGraphicsPipelineObject()
            targets = controller.GetDisassemblyTargets(True)
            text = controller.DisassembleShader(pipe, refl, targets[0] if targets else "")
            path = os.path.join(OUT, "shader_%d_%s.txt" % (ev, name))
            with open(path, "w") as f:
                f.write(text)
            say("event", ev, name, "resource", int(refl.resourceId), "->", path, len(text), "chars; targets", list(targets))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
