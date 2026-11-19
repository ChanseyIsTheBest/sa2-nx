// renut-ultimate (2026-11-16, port 145; tw2de_nx's render/fetch_filters.h of 2026-10-31, as it was;
// banjo_renut_fetch_filters, off by default at the user's word): a shader's texture fetch instructions' own
// filters.
//
// A Xenos texture fetch (tfetch) carries a mag, min and mip filter and an anisotropy of its own; each is either a
// value (point, linear, base map; an anisotropy ratio) or "use the fetch constant's" (3; 7 for the anisotropy). The
// hardware samples with the instruction's where it gives one. Renut built every sampler from the fetch constant
// alone, so a shader that asks for linear filtering on a texture whose fetch constant says point sampled it point:
// FINAL FANTASY XIII's light shafts (the radial blur of a 256x144 mask, pixel shader 1F7BE3E2C0AEDDA1: hard, stepped
// copies of the mask instead of soft rays, flickering as the camera moves).
// Read from the guest microcode when the shader object is made (no translation or compile changes): for each fetch
// constant the shader's tfetches use, the filter fields they all give (a constant whose fetches disagree is left to
// the fetch constant), as the bits of fetch constant word 3 they replace.
#pragma once

#include <cstdint>

namespace renut::gpu {

struct FetchFilterOverrides {
  uint32_t slots = 0;       // fetch constants with an override, bit r = constant r
  uint32_t clear[32] = {};  // word 3 bits replaced (mag 19-20, min 21-22, mip 23-24, anisotropy 25-27)
  uint32_t set[32] = {};    // their values
  uint32_t conflicts = 0;   // constants whose fetches give different filters (left alone)
  // tw2de_nx (2026-11-14): the constants any texture fetch instruction of the shader names (bit r: constant r);
  // every one if the container could not be read. (Its page look-ahead reads it; kept here for a later port.)
  uint32_t fetched = 0xFFFFFFFFu;

  bool Empty() const { return slots == 0; }
  // Word 3 of fetch constant r as this shader samples it.
  uint32_t Apply(uint32_t r, uint32_t word3) const {
    return (slots >> r) & 1u ? (word3 & ~clear[r]) | set[r] : word3;
  }
};

// The overrides of a shader container (the guest's: big-endian words, virtual part then physical part, their sizes
// in its header).
FetchFilterOverrides ScanFetchFilters(const uint8_t *container);

// A draw's: the vertex shader's, then the pixel shader's over them (a constant both give: the pixel shader's).
void MergeFetchFilters(FetchFilterOverrides &into, const FetchFilterOverrides &from);

}  // namespace renut::gpu
