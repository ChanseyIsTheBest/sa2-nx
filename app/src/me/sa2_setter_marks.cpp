// sa2-nx (2026-10-28, sa2_renut_setter_marks): hooks for the Direct3D setters that mark chunks (me/sa2_setter_marks.h)
// and had no hook of their own: the original runs unchanged, then the marks. The hooked ones (the hot hooks' macros,
// sa2_me_hooks.cpp) mark in their own hooks. With sa2_renut_setters_precise, also the readers of the dirty words below.

#include <rex/hook.h>
#include <rex/ppc.h>

#include "me/sa2_setter_marks.h"

#define SA2_SETTER_MARK_HOOK(ADDR)    \
  REX_EXTERN(__imp__sub_##ADDR);      \
  REX_HOOK_RAW(sub_##ADDR) {          \
    SA2_SETTER_MARKS(ADDR);           \
    __imp__sub_##ADDR(ctx, base);     \
  }

SA2_SETTER_MARK_HOOK(82552C48)
SA2_SETTER_MARK_HOOK(82552CF8)
SA2_SETTER_MARK_HOOK(82552D88)  // SetRenderTarget
SA2_SETTER_MARK_HOOK(82553118)  // SetDepthStencilSurface
SA2_SETTER_MARK_HOOK(82559600)  // the present
SA2_SETTER_MARK_HOOK(82550D98)
SA2_SETTER_MARK_HOOK(8255EFB8)
SA2_SETTER_MARK_HOOK(825525B0)  // SetIndices
SA2_SETTER_MARK_HOOK(8256FE38)
SA2_SETTER_MARK_HOOK(8256FB60)
SA2_SETTER_MARK_HOOK(8256DF00)
SA2_SETTER_MARK_HOOK(8256ABC0)
SA2_SETTER_MARK_HOOK(82568DB8)
SA2_SETTER_MARK_HOOK(8255DAE8)
SA2_SETTER_MARK_HOOK(8255DA88)
SA2_SETTER_MARK_HOOK(82558348)
SA2_SETTER_MARK_HOOK(82557A40)
SA2_SETTER_MARK_HOOK(825576D0)
SA2_SETTER_MARK_HOOK(82557560)
SA2_SETTER_MARK_HOOK(825573E0)
SA2_SETTER_MARK_HOOK(825553A8)
SA2_SETTER_MARK_HOOK(8254B108)
SA2_SETTER_MARK_HOOK(82549F98)
SA2_SETTER_MARK_HOOK(82549EA0)
SA2_SETTER_MARK_HOOK(8256E1D0)
SA2_SETTER_MARK_HOOK(8256E2C8)
SA2_SETTER_MARK_HOOK(825528A8)

// sa2-nx (2026-10-28, sa2_renut_setters_precise): the Direct3D functions that read m_Pending words 0-4 for more than
// an 'or' into them (scratchpad pending_readers2.py over the generated code) and had no hook: the dirty bits the precise
// setters hold apart go back into the device first, so they find the words as the original setters left them.
#define SA2_TAG_READER_HOOK(ADDR)        \
  REX_EXTERN(__imp__sub_##ADDR);         \
  REX_HOOK_RAW(sub_##ADDR) {             \
    sa2::marks::Materialise(ctx, base);  \
    __imp__sub_##ADDR(ctx, base);        \
  }

SA2_TAG_READER_HOOK(82549B10)
SA2_TAG_READER_HOOK(8254AB98)
SA2_TAG_READER_HOOK(8254BA90)
SA2_TAG_READER_HOOK(8254C140)
SA2_TAG_READER_HOOK(8254C560)
SA2_TAG_READER_HOOK(8254D360)
SA2_TAG_READER_HOOK(8254D3C8)
SA2_TAG_READER_HOOK(8254E0E0)
SA2_TAG_READER_HOOK(8254E220)
SA2_TAG_READER_HOOK(8254F6A8)  // clears word 4's two top bits (69,164 calls in the census session)
SA2_TAG_READER_HOOK(82556920)
SA2_TAG_READER_HOOK(8255B610)
SA2_TAG_READER_HOOK(8255D730)
SA2_TAG_READER_HOOK(8255E340)
SA2_TAG_READER_HOOK(82560AD0)
SA2_TAG_READER_HOOK(82564340)
SA2_TAG_READER_HOOK(82568550)
SA2_TAG_READER_HOOK(825685C0)
SA2_TAG_READER_HOOK(8256CE60)
SA2_TAG_READER_HOOK(8256DC68)
SA2_TAG_READER_HOOK(8256DF60)
SA2_TAG_READER_HOOK(8256EB28)
SA2_TAG_READER_HOOK(8256ECC0)
SA2_TAG_READER_HOOK(8256F1A0)
SA2_TAG_READER_HOOK(8256F3D0)
