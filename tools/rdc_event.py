# banjo-nx (2026-10-18): what became of chosen draws of a RenderDoc capture (run inside qrenderdoc):
#   qrenderdoc.exe --python tools/rdc_event.py
# Environment: RDC_IN (capture), RDC_OUT (folder for event.txt), RDC_EVENTS (comma list of event ids), RDC_PIXELS
# ("x,y;x,y" pixels of the draw's colour target 0 for a pixel history), RDC_VERTS (vertices to print, default 8).
# For each draw: depth/stencil and rasterizer state, the vertex shader output (clip and NDC ranges, NaN, w <= 0, the
# first vertices with all their outputs) and the pixel history of the pixels at that draw.
import math
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
PIXELS = [tuple(int(c) for c in p.split(",")) for p in os.environ.get("RDC_PIXELS", "").split(";") if p.strip()]
NVERTS = int(os.environ.get("RDC_VERTS", "8"))
log = open(os.path.join(OUT, "event.txt"), "w")


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
    sfile = controller.GetStructuredFile()
    actions = {a.eventId: a for a in flatten(controller.GetRootActions(), [])}
    for eid in EVENTS:
        a = actions.get(eid)
        if not a:
            say("event", eid, "not found")
            continue
        controller.SetFrameEvent(eid, True)
        st = controller.GetPipelineState()
        say("event", eid, a.GetName(sfile), "indices", a.numIndices, "instances", a.numInstances,
            "baseVertex", a.baseVertex, "indexOffset", a.indexOffset)
        try:
            vk = controller.GetVulkanPipelineState()
            ds = vk.depthStencil
            rs = vk.rasterizer
            say("   depth test", ds.depthTestEnable, "write", ds.depthWriteEnable, "func", ds.depthFunction,
                "bounds", ds.depthBoundsEnable, "stencil", ds.stencilTestEnable)
            say("   cull", rs.cullMode, "front ccw", rs.frontCCW, "depth clamp", rs.depthClampEnable,
                "discard", rs.rasterizerDiscardEnable, "fill", rs.fillMode, "topology", vk.inputAssembly.topology)
            vp = vk.viewportScissor.viewportScissors[0].vp
            say("   viewport %.1f %.1f %.1f %.1f depth %.3f..%.3f" % (vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth))
        except Exception as e:
            say("   state ?", e)
        # The pixel shader's textures as bound: resource, view format and swizzle.
        try:
            for used in st.GetReadOnlyResources(rd.ShaderStage.Pixel):
                d = used.descriptor
                if d.resource == rd.ResourceId.Null():
                    continue
                sw = getattr(d, "swizzle", None)
                swz = ("%s%s%s%s" % (sw.red, sw.green, sw.blue, sw.alpha)) if sw is not None else "?"
                fmt = d.format.Name() if hasattr(d, "format") else "?"
                say("   ps tex", int(d.resource), "bind", used.access.index if hasattr(used, "access") else "?",
                    "view format", fmt, "swizzle", swz.replace("TextureSwizzle.", ""))
        except Exception as e:
            say("   ps tex ?", e)
        # Storage and uniform buffers the vertex shader has bound: resource, offset, size and the first words there.
        for stage in (rd.ShaderStage.Vertex,):
            for kind, getter in (("ro", st.GetReadOnlyResources), ("rw", st.GetReadWriteResources),
                                 ("cb", st.GetConstantBlocks)):
                try:
                    items = getter(stage)
                except Exception as e:
                    say("   ", kind, "?", e)
                    continue
                for used in items:
                    d = used.descriptor
                    if d.resource == rd.ResourceId.Null():
                        continue
                    is_buffer = any(int(b.resourceId) == int(d.resource) for b in controller.GetBuffers())
                    if not is_buffer:
                        continue
                    raw = controller.GetBufferData(d.resource, d.byteOffset, 64)
                    words = struct.unpack_from("<%dI" % (len(raw) // 4), raw)
                    say("   vs", kind, "buffer", int(d.resource), "offset", hex(d.byteOffset), "size", hex(d.byteSize),
                        " ".join("%08X" % w for w in words))
        # Vertex inputs: each attribute's buffer, offset and format, and its first vertices' raw words.
        try:
            vbs = st.GetVBuffers()
            for attr in st.GetVertexInputs():
                fmt = attr.format.Name() if hasattr(attr.format, "Name") else str(attr.format)
                if not attr.used:
                    continue
                if attr.genericEnabled:
                    say("   in", attr.name, "generic", list(attr.genericValue.floatValue)[:4])
                    continue
                vb = vbs[attr.vertexBuffer] if attr.vertexBuffer < len(vbs) else None
                if vb is None or vb.resourceId == rd.ResourceId.Null():
                    say("   in", attr.name, "buffer", attr.vertexBuffer, "unbound")
                    continue
                vals = []
                for v in range(min(4, NVERTS)):
                    raw = controller.GetBufferData(vb.resourceId, vb.byteOffset + v * vb.byteStride + attr.byteOffset, 16)
                    if len(raw) >= 16:
                        vals.append("[%s]" % " ".join("%08X" % w for w in struct.unpack_from("4I", raw)))
                say("   in", attr.name, "buffer", attr.vertexBuffer, "stride", vb.byteStride, "offset", attr.byteOffset,
                    "fmt", fmt, "instance" if attr.perInstance else "", " ".join(vals))
        except Exception as e:
            say("   inputs ?", e)
        mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if mesh.vertexResourceId == rd.ResourceId.Null():
            say("   no post-VS data")
        else:
            data = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, 0)
            stride = mesh.vertexByteStride
            count = min(len(data) // stride, 65536) if stride else 0
            nan = wneg = 0
            lo = [1e30] * 4
            hi = [-1e30] * 4
            for v in range(count):
                x, y, z, w = struct.unpack_from("4f", data, v * stride)
                if any(math.isnan(c) for c in (x, y, z, w)):
                    nan += 1
                    continue
                for i, c in enumerate((x, y, z, w)):
                    lo[i] = min(lo[i], c)
                    hi[i] = max(hi[i], c)
                if w <= 0:
                    wneg += 1
            say("   post-VS vertices", count, "stride", stride, "NaN", nan, "w<=0", wneg,
                "clip x %.4g..%.4g y %.4g..%.4g z %.4g..%.4g w %.4g..%.4g" % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]))
            for v in range(min(NVERTS, count)):
                words = struct.unpack_from("%df" % (stride // 4), data, v * stride)
                say("   v%d" % v, " ".join("%.5g" % w for w in words))
        target = a.outputs[0] if len(a.outputs) else rd.ResourceId.Null()
        for (px, py) in PIXELS:
            if target == rd.ResourceId.Null():
                break
            hist = controller.PixelHistory(target, px, py, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            for m in hist:
                if m.eventId != eid:
                    continue
                flags = []
                for name in ("sampleMasked", "backfaceCulled", "depthClipped", "depthBoundsFailed", "viewClipped",
                             "scissorClipped", "shaderDiscarded", "depthTestFailed", "stencilTestFailed",
                             "predicationSkipped", "unboundPS", "directShaderWrite"):
                    if getattr(m, name, False):
                        flags.append(name)
                say("   pixel", px, py, "prim", m.primitiveID, "flags", flags or "passed",
                    "shader out %.4f %.4f %.4f %.4f" % tuple(m.shaderOut.col.floatValue[:4]),
                    "pre depth %.6f post depth %.6f" % (m.preMod.depth, m.postMod.depth),
                    "post %.4f %.4f %.4f %.4f" % tuple(m.postMod.col.floatValue[:4]))
            if not any(m.eventId == eid for m in hist):
                say("   pixel", px, py, "no fragment of this draw")
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
