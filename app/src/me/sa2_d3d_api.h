// Sonic Adventure 2: the game's Direct3D at its API boundary (see sa2_d3d_api.cpp).
#pragma once

#include <cstdint>

namespace sa2::d3d {

// The Direct3D device, from the Draw* calls' r3 (the census reads its command buffer pointers).
void NoteDevice(uint32_t device);
// Census: the device's flag bytes at a draw (a histogram in the report).
void NoteDrawFlags(const uint8_t* base, uint32_t device);

// sa2_diag_d3d_census: true when the census is on (read once).
bool CensusOn();
// Around a call of a census-hooked function (the renderer's own hooks call these too): counts the outermost call and
// the PM4 it wrote, or a nested call.
void CensusEnter(uint32_t address, const uint8_t* base);
void CensusLeave(const uint8_t* base);
// Around the command buffer's space function (a segment switch inside a call): the old segment's packets are decoded
// before it, the new segment is followed after it.
void CensusSegmentBefore(const uint8_t* base);
void CensusSegmentAfter(const uint8_t* base);

// sa2-nx (2026-10-28): the setter census (census builds, SA2_D3D_CENSUS_BUILD): each outermost census call's device
// lines (64 bytes) that it changed, per function; and at each draw, the delta chunks the capture found changed that no
// census-bracketed call changed since the previous draw on that thread: written by the game's own code (inline).
void CensusDrawTags(uint64_t states, uint64_t fetch, uint64_t misc);  // the dirty tags pending at the draw (words 2-4)
void CensusDrawChunk(uint32_t offset, uint32_t size);  // a chunk the draw's capture (CaptureDelta) found changed
void CensusDrawDone(uint32_t device);                   // after the draw's capture (its device, which the census then uses)
// Brackets a hook with CensusEnter / CensusLeave for a function of the Direct3D range (census builds' natives and
// copies, sa2_hot_guest.h): their device writes are then attributed to them, not to inline code.
struct CensusScope {
  bool on;
  const uint8_t* base;
  CensusScope(uint32_t address, const uint8_t* b) : on(address >= 0x82540000u && address < 0x82570000u && CensusOn()), base(b) {
    if (on) CensusEnter(address, base);
  }
  ~CensusScope() {
    if (on) CensusLeave(base);
  }
};

}  // namespace sa2::d3d

#if defined(SA2_D3D_CENSUS_BUILD)
#define SA2_D3D_CENSUS_SCOPE(ADDR) const sa2::d3d::CensusScope sa2_census_scope_{0x##ADDR##u, base}
#else
#define SA2_D3D_CENSUS_SCOPE(ADDR) ((void)0)
#endif

// The census build's hooks (generated into out/census, see sa2_d3d_api.cpp): the original runs unchanged.
#define SA2_D3D_CENSUS_HOOK(ADDR)                  \
  REX_EXTERN(__imp__sub_##ADDR);                   \
  REX_HOOK_RAW(sub_##ADDR) {                       \
    sa2::d3d::CensusEnter(0x##ADDR##u, base);      \
    __imp__sub_##ADDR(ctx, base);                  \
    sa2::d3d::CensusLeave(base);                   \
  }
#define SA2_D3D_CENSUS_SEGMENT_HOOK(ADDR)          \
  REX_EXTERN(__imp__sub_##ADDR);                   \
  REX_HOOK_RAW(sub_##ADDR) {                       \
    sa2::d3d::CensusSegmentBefore(base);           \
    __imp__sub_##ADDR(ctx, base);                  \
    sa2::d3d::CensusSegmentAfter(base);            \
  }
