/**
 * @file    gpu/guest/command_buffers.h
 * @brief   D3D command buffers: draws recorded on one device, run on another.
 *
 * N&B records parts of its frame on worker threads, each with its own device
 * between D3DDevice_BeginCommandBuffer and EndCommandBuffer, and runs the
 * buffers on the title device with D3DDevice_RunCommandBuffer. The hooked
 * draw, clear and resolve calls of a recording device are kept with a copy of
 * the device state they read, and replayed through the same draw path when
 * the buffer runs. The guest buffer still gets the originals' packets for the
 * calls that are not replaced; nothing reads them.
 *
 * Every buffer N&B records inherits the vertex and pixel shader float
 * constants: those it does not set itself come from the running device.
 *
 * The recorded buffers have a lock of their own, which these functions take;
 * none of them needs State().mutex. A run is queued for the render thread
 * with the buffers as they are at the call (a buffer recorded again later is
 * a new one), so the game may record the next frame's while it replays.
 */
#pragma once

#include <string>

#include <rex/types.h>

#include "renut_engine/gpu/render/draw.h"

namespace renut::gpu {

// True while the device records a command buffer.
bool IsRecordingCommandBuffer(u32 device_va);

// halo3-nx (2026-11-03, banjo_renut_recorded_binds; d3d/hooks/shader.cpp): a game thread, at a draw: the id of the
// D3DVertexShader_Bind its shader has now (0: none). The render thread, before that draw: the shader record takes it.
u32 LastBindingOf(u32 shader_va);
void ApplyBindingLocked(u32 id);

// After the original BeginCommandBuffer: drops what the buffer held before.
void BeginRecording(u32 device_va, u32 command_buffer_va);

// Records a draw on a recording device. 'up_vertex_bytes' / 'up_index_bytes'
// are the sizes of call.up_vertices / call.up_indices, copied now, as the GPU
// copies them into the buffer.
void RecordDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes);

// Records a Begin*Vertices draw whose 'bytes' of vertex data the game writes
// to 'scratch' after the call; copied at the device's next hooked call.
void RecordDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes);

void RecordClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4],
                 float depth, u32 stencil);

void RecordResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                   i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                   const float clear_color[4], float clear_depth, u32 clear_stencil);

// D3DDevice_RunCommandBuffer on a recording device: records the run.
void RecordRun(u32 device_va, u32 command_buffer_va);
// banjo-nx (2026-10-26, banjo_renut_diag_fast_census): replayed recordings against the frame before's; empty when none.
// (2026-10-29: its own name; it shared render/draw.h's, one definition of the two was linked.)
std::string ReplayRepeatCensusReport();
// banjo-nx (2026-10-28, banjo_renut_diag_packet_census): replayed draws a whole-state keyed cache would answer.
std::string PacketCensusReport();
std::string StateSourceReport();  // banjo-nx (2026-10-26, banjo_renut_diag_fast_census)
std::string RecordLockReport();  // blur-nx (2026-11-17): NewOp's memo census and the record lock's slow paths

// Copies the data of the device's recorded Begin*Vertices draw, if any. Any
// thread, with or without State().mutex.
void FlushRecordedVertices(u32 device_va);

// D3DDevice_RunCommandBuffer on a device that is not recording: queues the
// buffer's replay on it, with the device's state now (the constants the
// buffer inherits).
void QueueRunCommandBuffer(u32 device_va, u32 command_buffer_va);

void ReleaseCommandBuffer(u32 command_buffer_va);

// banjo-nx (2026-10-28, the heap): the spare buffers given back while the heap's room is below 256 MB (checked every
// 5 s; the frame's hook calls it).
void ReleaseSpareBuffersWhenHeapShort();
// banjo-nx (2026-10-28, the console's OOM): the spare buffers given back now (plume's memory pressure callback: a GPU
// memory block found no room in the heap). Any thread; takes only the spares' own lock.
void ReleaseSpareBuffersNow();

// For the memory report: the capacity of the recordings the game's command
// buffers hold now, and of the spare recordings kept for reuse; 'live'
// counts every recording, including older ones queued runs still hold.
struct CommandBufferMemory {
  size_t recorded_bytes = 0;
  size_t recorded = 0;
  size_t spare_bytes = 0;
  size_t spare = 0;
  size_t live = 0;
};
CommandBufferMemory CommandBufferMemoryUsage();

// banjo-nx (2026-10-26, banjo_renut_tags): the 10 s "[renut] recording from marks" line (empty with nothing recorded);
// resets its counts.
std::string TagsReport();
// banjo-nx (2026-10-27, banjo_renut_shadow_sort): the 10 s "[renut] shadow sort" line; resets its counts.
std::string ShadowSortReport();
// banjo-nx (2026-10-27, banjo_renut_shadow_sort): a shader object's content hash and whether its draws may be reordered
// (draw.h ShaderSortableByHash), where it is made; any thread.
void NoteShaderSortKey(u32 va, uint64_t hash, bool sortable);

// sasrt-nx (2026-10-21, sasrt_renut_constant_f4_pending): for a game's D3DDevice_BeginShaderConstantF4 hook, after the original
// succeeded: the range's float constant groups pending again on the device, so a recorded draw takes them from it.
void KeepConstantF4Pending(u32 device_va, bool pixel, u32 start, u32 count);

} // namespace renut::gpu
