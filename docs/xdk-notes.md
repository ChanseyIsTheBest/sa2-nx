# Renut at a later XDK: notes from other ports

The base's Renut stands at Beautiful Katamari's XDK 2.0.5632. These are what ports at a later XDK met, for the next
port's remap of the hooks and the device layout. Each is that port's finding on its own image; check it on yours.

## XDK 2.0.11164 (blur-nx, 2026-11-10)

Evidence: blur-nx's `docs/blur-notes.md`, "Renut at XDK 2.0.11164" (D:\blur\blur-nx), with the tools named there.

- **The first release's unused second argument.** D3D_Lock2DSurface, D3D_LockSurface, D3D_LockResource and
  D3DDevice_BeginCommandBuffer take one more argument than at 5632, unused. pacman-nx met three of them at XDK 21250;
  they are already there at 11164.
- **SetGammaRamp and SetPWLGamma** take (device, flags, ramp).
- **D3DVertexShader_Bind** is (shader r3, flags r4, declaration r5, stream strides r6, pixel shader r7). Katamari never
  links it, so it is one of the base's `BANJO_RENUT_UNHOOKED_*` bodies: left unhooked, Blur's ran on Renut's declaration
  record and walked off the guest stack. blur-nx hooks it through `D3DVertexShader_Bind11164_hook` (the flags and the
  pixel shader dropped: Renut keeps the declaration and the strides).
- **XGRegisterPixelShader** is linked and makes the pixel shaders where CreatePixelShader is not linked (Blur); its
  instructions write what Renut's hook writes.
- **The tiling Flags word.** pacman-nx's N&B -> 11164 device-offset table moves N&B's 13196 by +176; Blur's BeginTiling
  stores it at 13364, +168. Check that word against your BeginTiling.
- **The state tables.** `tools/renut_state_tables.py --ref-render 82AD8E48 --ref-sampler 82AD9308` pairs a game's render
  and sampler state tables with Katamari's (port 69's options); Blur's are 0x82B9A888 and 0x82B9AD48, all 121 records
  paired by their defaults.
- **Vertex shaders with two bindings.** Blur makes about 1,000 vertex shaders at run time whose container has flags bit
  0x20 (0x102A1121). XenosRecomp's dump reader skipped them before port 68.

## XDK 2.0.9407.0 (rehatched-nx, Angry Birds Trilogy, 2026-11-16; port 176)

What rehatched-nx found moving Renut's Direct3D hooks to this XDK (its docs/rehatched-notes.md section 4). Every
address below is Angry Birds'; a port finds its own (tools/xdk_sigmatch.py, the hook maps).

- **After any change to a hook address, run `tools/post_codegen.py` in full** (until `direct_calls.py --check`
  reports 0). Otherwise the calls to the moved functions stay direct `__imp__sub_X` calls that skip the hooks:
  rehatched-nx's device was made the XDK's way and its Swap waited forever for a ring pointer no GPU moves.
- **Forms with Flags.** D3DVertexShader_Bind is the first release's (pShader, Flags, pDecl, pStreamStrides, Index):
  bind `D3DVertexShader_Bind_flags_hook` (port 173), not the three-argument hook. SetGammaRamp and SetPWLGamma keep
  Flags, (D3DDevice*, Flags, ramp): bind `D3DDevice_SetGammaRamp_flags_hook` and `D3DDevice_SetPWLGamma_flags_hook`.
  A port whose own hooks of those names already take Flags (blur-nx at XDK 2.0.11164) leaves out port 173's
  block of two-argument `D3DDevice_SetGammaRamp_hook` / `D3DDevice_SetPWLGamma_hook` declarations and their
  Flags wrappers (d3d/hooks/device.cpp): with both, the names are overloaded and BANJO_RENUT_HOOK does not
  compile (Blur's 909f520b).
- **D3DVertexDeclaration_GetDeclaration.** A game that asks for a declaration's elements gets zeros from the XDK's
  own (Renut's declarations are records with no elements in their guest block), and a search for D3DDECL_END runs
  off the stack (Angry Birds' crash at the end of level 1). Bind `D3DVertexDeclaration_GetDeclaration_hook` (port 173).
- **The internal resolve.** D3DDevice_Resolve (Angry Birds' sub_8228CA80) is a wrapper that calls an internal resolve
  (sub_8228B928) with two extra arguments, 0 from the wrapper; the game's renderer also calls the internal one
  directly, so those resolves never reached Renut (black level backgrounds). rehatched-nx maps it onto Resolve's hook
  with a raw hook; its arguments: r3 device, r4 flags, r5 pSourceRect, r6 a value it writes in a PM4 register packet
  and r7 a flag (both dropped), r8 pDestTexture, r9 pDestPoint, r10 DestLevel; on the caller's stack +84
  DestSliceOrFace, +92 pClearColor, +108 ClearStencil, +116 pParameters; ClearZ in f1:

      BANJO_RENUT_HOOK_RAW(sub_<internal resolve>) {
        const u32 sp = ctx.r1.u32;
        D3DDevice_Resolve_hook(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                               LoadU32(sp + 84), LoadU32(sp + 92), ctx.f1.f64, 0, LoadU32(sp + 108),
                               LoadU32(sp + 116));
      }

  Check the layout against the port's own wrapper's call first. Katamari's XDK has no such split.
- **Device offsets** (d3d.h, d3d_native_state.cpp): vertex declaration +12240, pixel shader +13068, vertex shader
  +13072; the device's own (FVF) declaration +12568; the device allocation 0x6080.
- **Re-pointed headers.** Angry Birds re-points one surface header between three EDRAM views each frame: turn on
  `banjo_renut_all_surface_headers` (port 174) with `banjo_renut_chain_reuse` and `banjo_deko3d_scanout_handover` off.
