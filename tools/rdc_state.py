# katamari-nx (2026-11-01): the fixed-function state of chosen draws of a RenderDoc capture (run inside qrenderdoc,
# like rdc_passes.py): depth / stencil test and write, compare functions, blend, cull, the scissor and viewport, and
# the first vertex buffer's raw words in hex (a packed colour shows as a NaN float in rdc_mesh.py).
#   RDC_IN = capture, RDC_OUT = folder for state.txt, RDC_EVENTS = "e1,e2,..."
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = {int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()}
log = open(os.path.join(OUT, "state.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def fields(obj, names):
    out = []
    for n in names:
        try:
            out.append("%s=%s" % (n, getattr(obj, n)))
        except Exception:
            pass
    return " ".join(out)


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    for a in flatten(controller.GetRootActions(), []):
        if a.eventId not in EVENTS:
            continue
        controller.SetFrameEvent(a.eventId, True)
        say("event", a.eventId, "indices", a.numIndices)
        vk = controller.GetVulkanPipelineState()
        if vk is None:
            # blur-nx (2026-11-11): a D3D12 capture (renderdoccmd on Renut's D3D12 back end or the emulated renderer).
            d3d = controller.GetD3D12PipelineState()
            om = d3d.outputMerger
            dss = om.depthStencilState
            say("   depth", fields(dss, ["depthEnable", "depthWrites", "depthFunction", "depthBoundsEnable",
                                         "stencilEnable"]))
            say("   raster", fields(d3d.rasterizer.state, ["cullMode", "frontCCW", "depthClip", "depthBias",
                                                            "depthBiasClamp", "slopeScaledDepthBias", "fillMode",
                                                            "multisampleEnable"]))
            try:
                b = om.blendState.blends[0]
                say("   blend 0", fields(b, ["enabled"]), fields(b.colorBlend, ["source", "destination", "operation"]),
                    "alpha", fields(b.alphaBlend, ["source", "destination", "operation"]), "mask", b.writeMask)
            except Exception as e:
                say("   blend ?", e)
            try:
                vp = d3d.rasterizer.viewports[0]
                say("   viewport", fields(vp, ["x", "y", "width", "height", "minDepth", "maxDepth"]))
            except Exception as e:
                say("   viewport ?", e)
            continue
        ds = vk.depthStencil
        say("   depth", fields(ds, ["depthTestEnable", "depthWriteEnable", "depthFunction", "depthBoundsEnable",
                                    "stencilTestEnable"]))
        try:
            say("   stencil front", fields(ds.frontFace, ["function", "passOperation", "failOperation",
                                                         "depthFailOperation", "reference", "compareMask",
                                                         "writeMask"]))
        except Exception as e:
            say("   stencil ?", e)
        rs = vk.rasterizer
        say("   raster", fields(rs, ["cullMode", "frontCCW", "depthClampEnable", "depthBiasEnable",
                                     "depthBias", "slopeScaledDepthBias", "lineWidth", "fillMode"]))
        try:
            for i, b in enumerate(vk.colorBlend.blends[:1]):
                say("   blend", i, fields(b, ["enabled"]), fields(b.colorBlend, ["source", "destination",
                                                                               "operation"]),
                    "alpha", fields(b.alphaBlend, ["source", "destination", "operation"]),
                    "mask", b.writeMask)
        except Exception as e:
            say("   blend ?", e)
        try:
            vp = vk.viewportScissor
            for s in vp.viewportScissors[:1]:
                say("   viewport", fields(s.vp, ["x", "y", "width", "height", "minDepth", "maxDepth"]),
                    "scissor", fields(s.scissor, ["x", "y", "width", "height"]))
        except Exception as e:
            say("   viewport ?", e)
        state = controller.GetPipelineState()
        try:
            for i, vb in enumerate(state.GetVBuffers()[:3]):
                if vb.resourceId == rd.ResourceId.Null():
                    continue
                raw = controller.GetBufferData(vb.resourceId, vb.byteOffset, min(96, vb.byteSize or 96))
                words = struct.unpack_from("<%dI" % (len(raw) // 4), raw)
                say("   vb%d stride %d hex:" % (i, vb.byteStride), " ".join("%08x" % w for w in words[:24]))
        except Exception as e:
            say("   vb ?", e)
        try:
            for attr in state.GetVertexInputs()[:6]:
                say("   input", attr.name, attr.vertexBuffer, attr.byteOffset, attr.format.Name())
        except Exception as e:
            say("   inputs ?", e)
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
