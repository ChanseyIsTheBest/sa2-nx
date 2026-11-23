// banjo-nx (package 28b): banjo_renut_record_worker, Renut's renderer's command recording split across two threads.
//
// On the console the render thread was the limit (about 38 ms a frame at 1020 MHz), and a quarter of it was inside
// plume and NVK recording the Vulkan commands. While a command list is open the render thread now records into a
// proxy (the same plume::RenderCommandList calls, each one copied into a chunk of a command stream, its arrays with
// it), and a worker thread on another core plays the chunks on the real command list in the same order. Before the
// list is ended and submitted the render thread waits until the worker has played everything (Drain). The GPU gets
// the same commands in the same order, so the image is the same; what moves to the worker is the driver's work.
// banjo-nx's own renderer does the same (its deferred recording, -10-12 % a draw on the console in package 24b).
//
// banjo-nx (2026-11-21, the user's "build core 3"; banjo_deko3d_command_stage): the same on deko3d, the worker on the
// system core (core 3 in 4-core mode, ~70 % idle in Showdown Town) as the command stage: plume's deko3d command list
// and libdeko3d's encoding into command memory leave the render thread (about 12-14 % of it in console run c40). The
// renderer's deko3d calls that take the list (constant pushes, uniform and storage binds, the shared constants' CPU
// copy, waits, ...) go through the wrappers below, which record them on the proxy and call plume otherwise. Two things
// the list reads from the render thread's state are kept in stream order instead: the shared constants' copy that
// bindTextures reads (the stage keeps its own, from the recorded pushes to the shared buffer) and a descriptor set's
// image tables (plume's setTexture hands their update to the stream: Deko3DSetImageTrackHook).
#pragma once

#include <cstdint>

#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

namespace plume {
struct RenderCommandList;
}

namespace banjo::renut_gpu::record {

// Render thread, when a list is opened (Video::EnsureCommandListLocked): the list to record into, the proxy over
// 'real' when banjo_renut_record_worker is on (Vulkan; in-run A/B target 14 not in its off half) or
// banjo_deko3d_command_stage (deko3d, 4-core mode; A/B 182), else 'real'.
plume::RenderCommandList *Wrap(plume::RenderCommandList *real);
// Render thread: every command recorded so far played on the real list (before it is ended and submitted).
void Drain();
// SetBlendConstant (backend.cpp) calls Vulkan on the real command buffer itself: on the proxy it is a command too.
// True when 'cmd' is the proxy (the constant is then recorded).
bool SetBlendConstants(plume::RenderCommandList *cmd, const float rgba[4]);
// True when 'cmd' is the proxy: banjo_renut_tuning's NVK prefetch hint is then given by the worker before it binds.
bool IsProxy(const plume::RenderCommandList *cmd);
// banjo-nx (2026-11-21): the deko3d command stage plays the lists (its key, 4-core mode, not A/B 182's off half).
bool StageActive();
// banjo-nx (2026-11-22, banjo_deko3d_command_stage_float, A/B 189): the calling thread on cores 1-3 (core 3 preferred)
// or on core 3 alone as the key and the A/B half say; 'applied' is the caller's (0 at first), the system call made only
// when it changes. The stage's thread and the warm helper's call it in their loops.
void FollowStageCores(int &applied);
void Shutdown();
void Report(int seconds);  // the "[renut] record worker" line

// banjo-nx (2026-11-21, the next batch's item 5, banjo_deko3d_stage_submit): a call the stage makes in stream order
// (the list's end and its submit, the present): 'fn' gets a copy of the 'size' bytes at 'args' (at most 256), on the
// stage's thread. Returns the call's serial (1, 2, ...). Render thread, on the proxy (IsProxy).
uint64_t StageCall(void (*fn)(const void *args), const void *args, uint32_t size);
// Render thread: what is recorded so far handed to the stage now, without waiting for it.
void Hand();
// Render thread: until the stage has made call 'serial' (StageCall's answer; 0: none).
void WaitStageCall(uint64_t serial);
// banjo-nx (2026-11-21, batch 2, banjo_deko3d_stage_retired_free): while one is alive with 'played' true, plume's
// object destructors do not drain the stage: the objects retired with a frame slot whose every command the stage has
// played (its staged submit waited for, or drained at its submit) go at DrainSlot, and no command after names them.
// Render thread.
struct RetiredFreeScope {
  explicit RetiredFreeScope(bool played);
  ~RetiredFreeScope();
  RetiredFreeScope(const RetiredFreeScope &) = delete;
  RetiredFreeScope &operator=(const RetiredFreeScope &) = delete;
  bool on;
};

#if defined(PLUME_DEKO3D)
// The proxy (null until the first Wrap that returns it): the wrappers' test, inline.
extern plume::RenderCommandList *g_proxy;

namespace detail {
void PushUniform(uint64_t address, uint32_t uboSize, uint32_t offset, const void *data, uint32_t size);
void BindUniform(uint32_t slot, uint64_t address, uint32_t size);
void BindStorage(uint32_t slot, uint64_t address, uint32_t size);
void SetSharedWords(const void *cpu, uint32_t size);
void SharedLinesWritten(uint32_t lines);
void BindFragmentTextures(uint32_t first, const uint32_t *slots, uint32_t count);
void BindMemExport(const plume::RenderBuffer *buffer);
void BindFetchData(uint64_t address, uint32_t size);
void FlushL2();
void DiscardNextDraw();
void SetSampleMask(uint32_t mask);
void SetNextPrimitive(uint32_t primitive);
void BufferWait(uint32_t invalidate, bool countsAsBufferWait);
uint64_t BufferWaits();
void ReportCounter(const plume::RenderQueryPool *pool, uint32_t index, uint32_t counter);
// banjo-nx (2026-11-21, the next batch's item 3, banjo_deko3d_stage_swap): a constant push whose 'size' bytes the
// caller writes at the answer as the guest has them (big-endian words); the stage swaps them (and with 'nanFlush'
// zeroes a word whose exponent is all ones, as CopyAndSwapGroup does) and pushes them. The caller writes the bytes,
// then calls PushUniformRawEnd, with no other call on the proxy between. Render thread, on the proxy.
uint8_t *PushUniformRawBegin(uint64_t address, uint32_t uboSize, uint32_t offset, uint32_t size, bool nanFlush);
void PushUniformRawEnd();
}  // namespace detail

// plume's deko3d calls that take a command list, recorded when 'cmd' is the proxy (the bool ones then answer true: no
// caller reads the answer). Render thread.
inline bool PushUniform(plume::RenderCommandList *cmd, uint64_t address, uint32_t uboSize, uint32_t offset,
                        const void *data, uint32_t size) {
  if (cmd == g_proxy) {
    detail::PushUniform(address, uboSize, offset, data, size);
    return true;
  }
  return plume::Deko3DPushUniform(cmd, address, uboSize, offset, data, size);
}
inline bool BindUniform(plume::RenderCommandList *cmd, uint32_t slot, uint64_t address, uint32_t size) {
  if (cmd == g_proxy) {
    detail::BindUniform(slot, address, size);
    return true;
  }
  return plume::Deko3DBindUniform(cmd, slot, address, size);
}
inline bool BindStorage(plume::RenderCommandList *cmd, uint32_t slot, uint64_t address, uint32_t size) {
  if (cmd == g_proxy) {
    detail::BindStorage(slot, address, size);
    return true;
  }
  return plume::Deko3DBindStorage(cmd, slot, address, size);
}
inline bool SetSharedWords(plume::RenderCommandList *cmd, const void *cpu, uint32_t size) {
  if (cmd == g_proxy) {
    detail::SetSharedWords(cpu, size);
    return true;
  }
  return plume::Deko3DSetSharedWords(cmd, cpu, size);
}
inline void SharedLinesWritten(plume::RenderCommandList *cmd, uint32_t lines) {
  if (cmd == g_proxy)
    detail::SharedLinesWritten(lines);
  else
    plume::Deko3DSharedLinesWritten(cmd, lines);
}
inline bool BindFragmentTextures(plume::RenderCommandList *cmd, uint32_t first, const uint32_t *slots, uint32_t count) {
  if (cmd == g_proxy) {
    detail::BindFragmentTextures(first, slots, count);
    return true;
  }
  return plume::Deko3DBindFragmentTextures(cmd, first, slots, count);
}
inline bool BindMemExport(plume::RenderCommandList *cmd, const plume::RenderBuffer *buffer) {
  if (cmd == g_proxy) {
    detail::BindMemExport(buffer);
    return true;
  }
  return plume::Deko3DBindMemExport(cmd, buffer);
}
inline bool BindFetchData(plume::RenderCommandList *cmd, uint64_t address, uint32_t size) {
  if (cmd == g_proxy) {
    detail::BindFetchData(address, size);
    return true;
  }
  return plume::Deko3DBindFetchData(cmd, address, size);
}
inline void FlushL2(plume::RenderCommandList *cmd) {
  if (cmd == g_proxy)
    detail::FlushL2();
  else
    plume::Deko3DFlushL2(cmd);
}
inline void DiscardNextDraw(plume::RenderCommandList *cmd) {
  if (cmd == g_proxy)
    detail::DiscardNextDraw();
  else
    plume::Deko3DDiscardNextDraw(cmd);
}
inline void SetSampleMask(plume::RenderCommandList *cmd, uint32_t mask) {
  if (cmd == g_proxy)
    detail::SetSampleMask(mask);
  else
    plume::Deko3DSetSampleMask(cmd, mask);
}
inline void SetNextPrimitive(plume::RenderCommandList *cmd, uint32_t primitive) {
  if (cmd == g_proxy)
    detail::SetNextPrimitive(primitive);
  else
    plume::Deko3DSetNextPrimitive(cmd, primitive);
}
inline void BufferWait(plume::RenderCommandList *cmd, uint32_t invalidate, bool countsAsBufferWait) {
  if (cmd == g_proxy)
    detail::BufferWait(invalidate, countsAsBufferWait);
  else
    plume::Deko3DBufferWait(cmd, invalidate, countsAsBufferWait);
}
// The list's buffer barriers so far. On the proxy: those recorded through it, counted across its lists (a barrier
// recorded later is later on the GPU's one queue, whatever list it is in): the two sasrt tests that read it keep a wait
// at most once more than they would on the real lists.
inline uint64_t BufferWaits(const plume::RenderCommandList *cmd) {
  if (cmd == g_proxy)
    return detail::BufferWaits();
  return plume::Deko3DBufferWaits(cmd);
}
inline void ReportCounter(plume::RenderCommandList *cmd, const plume::RenderQueryPool *pool, uint32_t index,
                          uint32_t counter) {
  if (cmd == g_proxy)
    detail::ReportCounter(pool, index, counter);
  else
    plume::Deko3DReportCounter(cmd, pool, index, counter);
}
#endif  // PLUME_DEKO3D

}  // namespace banjo::renut_gpu::record
