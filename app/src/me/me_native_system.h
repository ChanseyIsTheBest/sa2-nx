// Native graphics system (see me_native_system.cpp).
#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

#include "me/banjo_records.h"

namespace me::native {

// true when MASSEFFECT_NATIVE=1: the app installs the native system instead of Xenos emulation.
bool Enabled();

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

// Called on the game thread when Direct3D binds a shader object (sub_8222F850), before it patches the
// vertex shader: records which library entry the microcode at that physical address belongs to, so the
// ring can identify IM_LOADs by address No-op without the native system.
void NoteShaderObject(const uint8_t* base, uint32_t object, bool vertex);

// True when the shader object was matched to a library entry (diagnostics).
bool HasEntry(uint32_t object);

// Called on the game thread by the four Direct3D draw functions (src/me_d3d_trace.cpp): the bound VS
// and PS objects and the call's primitive type and counts. The ring pairs each DRAW_INDX with these
// records in order, because the ring's own shader loads can be immediate
// copies Direct3D patched. No-op without the native system.
void NoteDrawCall(const uint8_t* base, uint32_t type, uint32_t r5, uint32_t r6, uint32_t r7);

// banjo_direct_state (the direct renderer, step 1, from sa2-nx): the draw hooks call BeginDrawDirect before Direct3D's
// draw function and EndDrawDirect right after it. The state packets the draw function wrote before its draw packet
// become a host record the ring applies directly (no PM4 parsing for them), covered in the command buffer by one NOP
// packet. See namespace direct in me_native_system.cpp. No-op without the native system or with the key off.
//
// banjo_d3d_records (step 4 of docs/d3d-full-replacement.md): the same for every Direct3D function that writes into a
// command buffer, with all the packets of the call (not only a draw's state): the draw hooks, the Swap, Resolve and
// the generated hooks of tools/d3d/d3d_record_hooks.py (banjo_d3d_record_hooks.inc). `depth` is a counter in the
// calling thread's guest context (PPCContext::host_counters[63]): only the outermost hooked call makes records, of
// everything written during it.
struct DrawDirect {
  uint32_t device = 0;       // 0 = nothing to do after the call
  uint32_t start = 0;        // Direct3D's command buffer write pointer before the call (the last word written)
  uint32_t limit = 0;        // the command buffer segment's limits before the call (device+52, +56): a segment
  uint32_t soft_limit = 0;   //   change during the call leaves the packets as they are
  uint64_t submissions = 0;  // the ring write pointer's guest writes before the call
  bool counted = false;      // `depth` was raised (EndDrawDirect lowers it)
  uint32_t writer = 0;       // the hooked function (diagnostics)
  uint32_t slot = 0, epoch = 0;  // banjo_d3d_records: the device's slot + 1 and this call's number on it
  bool alone = false;            //   no other call on the device was in progress when it began
  bool recording = false;        //   a recording device (its segments are never submitted by a kick)
  void* scope = nullptr;         //   the calling thread's segment scope (read once: a thread-local is a call on the Switch)
  // banjo_d3d_records_native (step 5): where the native draw path writes its record's operations (this device's
  // staging area); `ops.cur` null after the call: decode the region instead.
  banjo::records::RecordOps ops;
};
inline constexpr uint32_t kRecordDepthSlot = 63;  // PPCContext::host_counters slot
DrawDirect BeginDrawDirect(const uint8_t* base, uint32_t device, uint32_t& depth, uint32_t writer = 0);
// A writer that is not a draw function: `r3` is its device when it is a known one, else the device Swap is called with.
DrawDirect BeginWriterDirect(const uint8_t* base, uint32_t r3, uint32_t& depth, uint32_t writer = 0);
void EndDrawDirect(uint8_t* base, const DrawDirect& d, uint32_t& depth);
// D3DDevice_Swap's device (banjo_d3d_records: the writers' device when their r3 is not a device).
void NoteMainDevice(uint32_t device);
// After Direct3D's space function (sub_821DFFF0: the kick and the next segment) gave `device` a new segment, inside a
// hooked call (`depth` > 0): the call's records are made of what it writes in the new segment.
// `old_end`: the device's write pointer before the space function ran (the call's last word in the old segment).
void NoteSegmentSwitch(const uint8_t* base, uint32_t device, uint32_t depth, uint32_t old_end);
// banjo_d3d_host_submit (step 6): Direct3D's kick (sub_821DF948: r3 the device, r4 its list of segments as (length,
// address) pairs, r5 how many), around the original. KickBegin returns true when KickEnd must be called.
bool KickBegin(const uint8_t* base, uint32_t device, uint32_t list, uint32_t count);
void KickEnd();
// sub_8263FD60 (r3 the device, r4 the words it copies into the primary ring, r5 how many), before the original.
void RingWords(const uint8_t* base, uint32_t device, uint32_t words, uint32_t count);

// Called by the ring thread after it writes the read pointer back: wakes the game's D3D GPU wait
// (src/native/me_ring_wait.cpp, sub_821FEBE0).
void NotifyRingProgress();

// banjo-nx: one iteration of the game's D3D wait for ring space (Optimization_Hook, sub_8225D370): sleeps until the
// ring thread makes progress the game has not seen yet, or masseffect_wait_ring_max_us (src/me/me_ring_wait.cpp).
void WaitRingProgress();

}  // namespace me::native
