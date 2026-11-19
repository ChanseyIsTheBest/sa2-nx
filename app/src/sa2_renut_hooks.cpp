// sa2-nx (2026-10-20); sa2-nx-ult (2026-11-19): SA2's own hooks around Renut's renderer, from 1.0.6's
// src/me/sa2_me_hooks.cpp (its native-renderer hooks stay behind with that renderer: docs/ultimate-rebase-plan.md).
//
// The window scissor. Renut's draws take the window offset and scissor (registers 0x2080-0x2082) from the device's
// window packet (+0x28C0), but SA2's Direct3D writes them only into the command buffer (sub_8254FCE8, which
// SetViewport, SetScissorRect, the target setters, Clear and Resolve call: it keeps no shadow of them; N&B's, which
// Renut was written for, does). The shadow gets what the original writes in the untiled case (offset 0, then x / y of
// the two corners), then the original runs (its packets go to Renut's ring sink).

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/ppc.h>

#include "renut_engine/gpu/banjo_renut_select.h"

REX_EXTERN(__imp__sub_8254FCE8);
REX_HOOK_RAW(sub_8254FCE8) {
  if (::banjo::renut_gpu::Active()) {
    const uint32_t d = ctx.r3.u32;
    const uint32_t x0 = ctx.r4.u32, y0 = ctx.r5.u32, x1 = ctx.r6.u32, y1 = ctx.r7.u32;
    const uint32_t words[3] = {0u, ((y0 << 16) & 0x3FFF0000u) | (x0 & 0x3FFFu),
                               ((y1 << 16) & 0x3FFF0000u) | (x1 & 0x3FFFu)};
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t v = __builtin_bswap32(words[i]);
      std::memcpy(base + d + 0x28C0 + 4 * i, &v, 4);
    }
  }
  __imp__sub_8254FCE8(ctx, base);
}
