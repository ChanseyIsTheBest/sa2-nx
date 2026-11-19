/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#if defined(__SWITCH__)
#include <switch.h>  // blur-nx (2026-11-13, M-37): armGetSystemTick
#endif

extern "C" {
#include "libavutil/cpu.h"
#include "libavutil/log.h"
}  // extern "C"

REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, "Audio", "Verbose FFmpeg output (debug and above)");
REXCVAR_DEFINE_BOOL(audio_ffmpeg_simd, true, "Audio",
                    "Diagnostic: false = the FFmpeg XMA decoder uses C code only, without "
                    "NEON or SSE (to compare the audio)");
REXCVAR_DECLARE(bool, audio_diag_critical_priority);  // defined in audio_system.cpp
/*
 * On by default only on the Switch, like the server priority and the frame wait: there the game's
 * audio server thread runs at 0x2D, below the GPU ring (0x2C), and the ring preempts it in the middle
 * of decoding: 17.3 % of a core and peaks of 14.39 ms with a 10.7 ms buffer cushion (console). The
 * worker runs at 0x2B, above the ring.
 * Validated on the PC: the game thread goes from 167-302 ms to 10.7-16.1 ms per 10 s, without decoding
 * a single context, and the data is ready in 14-16 us on average (0.51 ms worst case), far below the
 * 5.3 ms of an audio block. 0 differences from the recompiled code.
 */
#if REX_PLATFORM_SWITCH
#define REX_XMA_IN_WORKER_BY_DEFAULT 1
#else
#define REX_XMA_IN_WORKER_BY_DEFAULT 0
#endif
// blur-nx (2026-11-13, #65's exact part): see g_worker_waiting.
REXCVAR_DEFINE_BOOL(audio_xma_kick_wake_idle_only, false, "Audio",
                    "XMA (audio_xma_in_worker 1 or 2): a kick signals the XMA Decoder thread only when it waits; a busy "
                    "worker finds the kick's marks in its next pass (no wake is lost: a handshake on the marks). The "
                    "same decodes, fewer signals")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(audio_xma_in_worker, REX_XMA_IN_WORKER_BY_DEFAULT, "Audio",
                     "Who decodes the XMA when a kick arrives (XMAEnableContext): 0 = the requesting thread, "
                     "as before; 1 = the XMA Decoder thread, which on the Switch runs at 0x2B and is not preempted by "
                     "the GPU ring; 2 = the worker, and the thread that requests the kick only finishes the "
                     "contexts the worker has not picked up (for the PC, where those priorities do not exist)");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal XmaDecoder handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc
//
// XMA details:
// https://devel.nuclex.org/external/svn/directx/trunk/include/xma2defs.h
// https://github.com/gdawg/fsbext/blob/master/src/xma_header.h
//
// XAudio2 uses XMA under the covers, and seems to map with the same
// restrictions of frame/subframe/etc:
// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.xaudio2.xaudio2_buffer(v=vs.85).aspx
//
// XMA contexts are 64b in size and tight bitfields. They are in physical
// memory not usually available to games. Games will use MmMapIoSpace to get
// the 64b pointer in user memory so they can party on it. If the game doesn't
// do this, it's likely they are either passing the context to XAudio or
// using the XMA* functions.

namespace rex::audio {

XmaDecoder::XmaDecoder(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()), function_dispatcher_(function_dispatcher) {}

XmaDecoder::~XmaDecoder() = default;

void av_log_callback(void* avcl, int level, const char* fmt, va_list va) {
  if (!REXCVAR_GET(ffmpeg_verbose) && level > AV_LOG_WARNING) {
    return;
  }

  string::StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.to_string_view();

  switch (level) {
    case AV_LOG_ERROR:
      REXAPU_ERROR("ffmpeg: {}", msg);
      break;
    case AV_LOG_WARNING:
      REXAPU_WARN("ffmpeg: {}", msg);
      break;
    case AV_LOG_INFO:
      REXAPU_INFO("ffmpeg: {}", msg);
      break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG:
    default:
      REXAPU_DEBUG("ffmpeg: {}", msg);
      break;
  }
}

X_STATUS XmaDecoder::Setup(system::KernelState* kernel_state) {
  // Setup ffmpeg logging callback
  av_log_set_callback(av_log_callback);
  // Diagnostic (audio_ffmpeg_simd): before opening any codec, make FFmpeg's function tables (FFT,
  // MDCT, float_dsp) be chosen without SIMD.
  if (!REXCVAR_GET(audio_ffmpeg_simd)) {
    av_force_cpu_flags(0);
    REXAPU_INFO("XMA: FFmpeg without SIMD (audio_ffmpeg_simd = false)");
  }

  // Register APU/XMA MMIO handlers
  // XMA registers are at 0x7FEA0000-0x7FEAFFFF
  memory()->AddVirtualMappedRange(
      0x7FEA0000,  // base address
      0xFFFF0000,  // mask
      0x0000FFFF,  // size (64KB)
      this,        // context (XmaDecoder*)
      reinterpret_cast<runtime::MMIOReadCallback>(MMIOReadRegisterThunk),
      reinterpret_cast<runtime::MMIOWriteCallback>(MMIOWriteRegisterThunk));
  REXAPU_DEBUG("XMA: Registered MMIO handlers at 0x7FEA0000-0x7FEAFFFF");

  // Setup XMA context data.
  // The Xbox 360 kernel allocates the contexts with X_PAGE_NOCACHE |
  // X_PAGE_READWRITE and writes MmGetPhysicalAddress for the address to the
  // register.
  context_data_first_ptr_ = memory()->SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA) * kContextCount, 256,
                                                      memory::kSystemHeapPhysical);
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XmaRegister::ContextArrayAddress] =
      memory()->GetPhysicalAddress(context_data_first_ptr_);

  // Setup XMA contexts.
  for (size_t i = 0; i < kContextCount; ++i) {
    uint32_t guest_ptr = context_data_first_ptr_ + i * sizeof(XMA_CONTEXT_DATA);
    XmaContext& context = contexts_[i];
    if (context.Setup(i, memory(), guest_ptr)) {
      assert_always();
    }
  }
  register_file_[XmaRegister::NextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);

  worker_running_ = true;
  work_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(work_event_);
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("XMA Decoder");

  worker_thread_->Create();
#if REX_PLATFORM_WIN32
  // Diagnostic audio_diag_critical_priority: as on the Switch, above the game threads
  // (THREAD_PRIORITY_TIME_CRITICAL = 15).
  if (REXCVAR_GET(audio_diag_critical_priority) && worker_thread_->thread()) {
    worker_thread_->thread()->set_priority(15);
  }
#endif

  return X_STATUS_SUCCESS;
}

/*
 * Measuring the XMA cost on the calling thread.
 *
 * The kick (XMAEnableContext) decodes right here, on the thread that requests it (the game's
 * audio server thread), and the lock (XMADisableContext) waits for the context lock if the worker
 * holds it. On the PC, the guest routine that does both once per voice takes 15 % of that thread's CPU,
 * with peaks of 22 ms in 500 ms windows.
 *
 * This changes nothing: every 10 s it logs how many kicks and locks there were, how many contexts and
 * how much time went into them, so decisions can be based on console numbers.
 */
namespace {

// blur-nx (2026-11-13, M-39 of Blur's docs/cpu-moonshots-50-1113.md): the counters by writer, each set on its own cache
// line. They were one struct written by the XMA Decoder thread (per decoded context) and by the guest threads that write
// the kick and lock registers (1,555 kicks a second): each add an exclusive load/store loop (the A57 has no LSE atomics)
// on a line the other core had just written. Now:
//   MeasurementXma:       the kick and lock side (any guest thread: atomic adds as before) and the report's clock;
//   MeasurementXmaWorker: the worker's, written by the XMA Decoder thread alone (a load and a store, no exclusive loop),
//                         never reset: the report takes the difference since its last; its maxima restart when the
//                         report moves to the next period (report_epoch).
// The log lines are the same.
struct alignas(64) MeasurementXma {
  std::atomic<uint64_t> kicks{0};
  std::atomic<uint64_t> kick_contexts{0};
  std::atomic<uint64_t> kick_ns{0};
  std::atomic<uint64_t> kick_max_ns{0};
  std::atomic<uint64_t> locks{0};
  std::atomic<uint64_t> lock_contexts{0};
  std::atomic<uint64_t> lock_ns{0};
  std::atomic<uint64_t> lock_max_ns{0};
  std::atomic<uint64_t> contexts_backup{0};  // the ones the kick thread had to finish (audio_xma_in_worker)
};

struct alignas(64) MeasurementXmaReport {
  std::atomic<int64_t> last_ms{0};
  std::atomic<uint32_t> report_epoch{0};  // the report's periods (the worker's maxima restart at a new one)
  // The worker's totals at the last report (the reporter of a period alone writes them).
  std::atomic<uint64_t> seen_contexts{0}, seen_work_ns{0}, seen_latency_ns{0}, seen_latency_n{0};
};

struct alignas(64) MeasurementXmaWorker {  // written by the XMA Decoder thread alone
  std::atomic<uint64_t> contexts_worker{0};  // decoded by the XMA Decoder thread
  std::atomic<uint64_t> work_ns{0};          // time the worker spends decoding
  std::atomic<uint64_t> work_max_ns{0};
  std::atomic<uint64_t> latency_ns{0};  // from the kick until the context is decoded
  std::atomic<uint64_t> latency_max_ns{0};
  std::atomic<uint64_t> latency_n{0};
  uint32_t epoch = 0;  // the report period its maxima are of (the worker's own)
};

MeasurementXma g_measurement_xma;
MeasurementXmaReport g_measurement_xma_report;
MeasurementXmaWorker g_measurement_xma_worker;

// blur-nx (2026-11-13, M-39): the worker's updates (its thread alone): a plain load and store.
void AddOwn(std::atomic<uint64_t>& counter, uint64_t value) {
  counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

void MaxOwn(std::atomic<uint64_t>& max, uint64_t value) {
  if (value > max.load(std::memory_order_relaxed)) {
    max.store(value, std::memory_order_relaxed);
  }
}

// The worker's maxima restart when the report has moved to a new period.
void WorkerPeriod() {
  MeasurementXmaWorker& w = g_measurement_xma_worker;
  const uint32_t epoch = g_measurement_xma_report.report_epoch.load(std::memory_order_relaxed);
  if (epoch != w.epoch) {
    w.epoch = epoch;
    w.work_max_ns.store(0, std::memory_order_relaxed);
    w.latency_max_ns.store(0, std::memory_order_relaxed);
  }
}

/*
 * Contexts a kick has just enabled that the worker has not served yet: 320 bits in 10 words.
 * Marking is one atomic operation and walking them takes no lock, so the game thread pays almost
 * nothing to notify. The kick time is stored to measure the latency until the data is ready.
 */
constexpr uint32_t kPendingWords = 10;  // 320 contexts / 32
std::atomic<uint32_t> g_pending[kPendingWords];
std::atomic<int64_t> g_pending_from_ns{0};

// blur-nx (2026-11-13, #65 of Blur's docs/cpu-near-exact-50-1113.md, its exact part): the worker is about to wait (or
// waits) on work_event_. A kick marks its contexts, then signals the event only when this is set
// (audio_xma_kick_wake_idle_only): a worker that is not waiting finds the marks in its next pass. No wake is lost: the
// worker sets this, then looks at the marks again before it waits; the kick sets its marks, then reads this; all four
// sequentially consistent, so at least one of them sees the other's write. Blur's console: 6,500 kicks a second,
// 27 ms a second on the kicking threads, mostly the event's signal.
std::atomic<bool> g_worker_waiting{false};
std::atomic<uint64_t> g_kick_wakes_skipped{0};  // the [xma] report's count

bool AnyPending() {
  for (uint32_t word = 0; word < kPendingWords; ++word)
    if (g_pending[word].load(std::memory_order_seq_cst)) return true;
  return false;
}

// blur-nx (2026-11-13, M-37 of docs/cpu-moonshots-50-1113.md): the measurements' clock. On the Switch the system tick
// (cntpct, 19.2 MHz; armTicksToNs is a multiply and a division by a constant), where steady_clock::now() went through
// clock_gettime per kick, per lock and per context a pass (0.15 ms a frame of samples, all at its entry). The same
// monotonic time base.
int64_t NowNs() {
#if defined(__SWITCH__)
  return int64_t(armTicksToNs(armGetSystemTick()));
#else
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

void MarkPending(uint32_t context_id) {
  // blur-nx (2026-11-13, #65): sequentially consistent, for g_worker_waiting's handshake (it was release).
  g_pending[context_id / 32].fetch_or(1u << (context_id % 32), std::memory_order_seq_cst);
}

// Returns true if this thread takes the context; only one can.
bool TakePending(uint32_t context_id) {
  const uint32_t bit = 1u << (context_id % 32);
  return (g_pending[context_id / 32].fetch_and(~bit, std::memory_order_acquire) & bit) != 0;
}

int64_t NowMsXma() {  // blur-nx (2026-11-13, M-39): from NowNs (the system tick on the Switch; read at every kick and lock)
  return NowNs() / 1000000;
}

uint64_t FromNs(int64_t before) {  // blur-nx (2026-11-13, M-37): from NowNs()
  return uint64_t(NowNs() - before);
}

void MaxXma(std::atomic<uint64_t>& max, uint64_t value) {
  uint64_t previous = max.load(std::memory_order_relaxed);
  while (value > previous && !max.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
  }
}

void ReportXma() {
  const int64_t now = NowMsXma();
  int64_t last = g_measurement_xma_report.last_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    g_measurement_xma_report.last_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10000 ||
      !g_measurement_xma_report.last_ms.compare_exchange_strong(last, now, std::memory_order_acq_rel)) {
    return;
  }
  const uint64_t kicks = g_measurement_xma.kicks.exchange(0, std::memory_order_relaxed);
  const uint64_t kick_contexts = g_measurement_xma.kick_contexts.exchange(0, std::memory_order_relaxed);
  const uint64_t kick_ns = g_measurement_xma.kick_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t kick_max = g_measurement_xma.kick_max_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t locks = g_measurement_xma.locks.exchange(0, std::memory_order_relaxed);
  const uint64_t lock_contexts = g_measurement_xma.lock_contexts.exchange(0, std::memory_order_relaxed);
  const uint64_t lock_ns = g_measurement_xma.lock_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t lock_max = g_measurement_xma.lock_max_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t of_the_backup = g_measurement_xma.contexts_backup.exchange(0, std::memory_order_relaxed);
  // blur-nx (2026-11-13, M-39): the worker's since the last report (its totals are never reset), then a new period.
  MeasurementXmaWorker& w = g_measurement_xma_worker;
  MeasurementXmaReport& r = g_measurement_xma_report;
  const auto since = [](const std::atomic<uint64_t>& total, std::atomic<uint64_t>& seen) {
    const uint64_t now_total = total.load(std::memory_order_relaxed);
    return now_total - seen.exchange(now_total, std::memory_order_relaxed);
  };
  const uint64_t of_the_worker = since(w.contexts_worker, r.seen_contexts);
  const uint64_t work_ns = since(w.work_ns, r.seen_work_ns);
  const uint64_t latency_ns = since(w.latency_ns, r.seen_latency_ns);
  const uint64_t latency_n = since(w.latency_n, r.seen_latency_n);
  const uint64_t work_max = w.work_max_ns.load(std::memory_order_relaxed);
  const uint64_t latency_max = w.latency_max_ns.load(std::memory_order_relaxed);
  r.report_epoch.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("[xma] on the calling thread, {:.1f} s: kicks {} with {} contexts decoded, {:.1f} ms (max "
              "{:.2f} ms); locks {} with {} contexts, {:.1f} ms waiting (max {:.2f} ms)",
              double(now - last) / 1000.0, kicks, kick_contexts, double(kick_ns) / 1e6, double(kick_max) / 1e6,
              locks, lock_contexts, double(lock_ns) / 1e6, double(lock_max) / 1e6);
  REXLOG_INFO("[xma] split (mode {}): worker {} contexts in {:.1f} ms (max {:.2f} ms), fallback of the "
              "kick thread {}; from the kick to the data ready mean {:.0f} us and max {:.2f} ms ({} measurements); "
              "kick wakes skipped (the worker not waiting: audio_xma_kick_wake_idle_only) {}",
              REXCVAR_GET(audio_xma_in_worker), of_the_worker, double(work_ns) / 1e6,
              double(work_max) / 1e6, of_the_backup,
              latency_n ? double(latency_ns) / double(latency_n) / 1e3 : 0.0, double(latency_max) / 1e6,
              latency_n, g_kick_wakes_skipped.exchange(0, std::memory_order_relaxed));
}

}  // namespace

void XmaDecoder::WorkerThreadMain() {
  while (worker_running_) {
    // Okay, let's loop through XMA contexts to find ones we need to decode!
    bool did_work = false;
    // First the ones a kick has just enabled: they are reached without walking all 320 or touching their
    // locks.
    bool pending_was = false;
    for (uint32_t word = 0; word < kPendingWords && worker_running_; ++word) {
      uint32_t bits = g_pending[word].load(std::memory_order_acquire);
      uint32_t index = 0;
      while (rex::bit_scan_forward(bits, &index)) {
        const uint32_t n = word * 32 + index;
        bits &= ~(1u << index);
        if (!TakePending(n)) {
          continue;  // the kick thread took it (mode 2)
        }
        pending_was = true;
        const int64_t before = NowNs();
        XmaContext& context = contexts_[n];
        const bool worked = context.Work();
        const uint64_t ns = FromNs(before);
        if (worked) {
          context.SignalWorkDone();
          PROFILE_XMA_FRAME_DECODED();
          WorkerPeriod();  // blur-nx (2026-11-13, M-39): the worker's own counters
          AddOwn(g_measurement_xma_worker.contexts_worker, 1);
          AddOwn(g_measurement_xma_worker.work_ns, ns);
          MaxOwn(g_measurement_xma_worker.work_max_ns, ns);
        }
        did_work = did_work || worked;
      }
    }
    if (pending_was) {
      // From the kick to the data being written: the latency the game could notice.
      const int64_t from = g_pending_from_ns.load(std::memory_order_acquire);
      if (from) {
        const uint64_t latency = uint64_t(NowNs() - from);
        WorkerPeriod();  // blur-nx (2026-11-13, M-39)
        AddOwn(g_measurement_xma_worker.latency_ns, latency);
        AddOwn(g_measurement_xma_worker.latency_n, 1);
        MaxOwn(g_measurement_xma_worker.latency_max_ns, latency);
      }
    }
    // Full sweep as a safety net: it is how it used to work, and it serves any context left enabled
    // without a mark. Only when there were no pending ones, so the 320 locks are not walked after every
    // kick and the fast loop keeps the work being counted.
    // blur-nx (2026-11-11, CPU batch 2): only the contexts the bitmap has enabled (XmaEnabledWord; Work() returns at
    // once for the others, after loading their flags: 320 cache lines a sweep, about 5 % of a core in the console
    // race of the LTO+PGO build). BLUR_XMA_SWEEP_ALL (compile time): every context, as before.
    if (!pending_was) {
#if !defined(BLUR_XMA_SWEEP_ALL)
      for (uint32_t word = 0; word < kContextCount / 32 && worker_running_; ++word) {
        uint32_t bits = XmaEnabledWord(word);
        uint32_t index = 0;
        while (bits && rex::bit_scan_forward(bits, &index)) {
          bits &= ~(1u << index);
          XmaContext& context = contexts_[word * 32 + index];
#else
      for (uint32_t n = 0; n < kContextCount && worker_running_; n++) {
        {
          XmaContext& context = contexts_[n];
#endif
          bool worked = context.Work();
          if (worked) {
            context.SignalWorkDone();
            PROFILE_XMA_FRAME_DECODED();
          }
          did_work = did_work || worked;
        }
      }
    }

    if (paused_) {
      pause_fence_.Signal();
      resume_fence_.Wait();
    }

    if (did_work) {
      continue;
    }
    // No work done this iteration, block until signaled.
    // blur-nx (2026-11-13, #65): announce the wait, then look at the marks once more (g_worker_waiting).
    g_worker_waiting.store(true, std::memory_order_seq_cst);
    if (AnyPending()) {
      g_worker_waiting.store(false, std::memory_order_relaxed);
      continue;
    }
    rex::thread::Wait(work_event_.get(), false);
    g_worker_waiting.store(false, std::memory_order_relaxed);
  }
}

void XmaDecoder::Shutdown() {
  if (!worker_thread_) {
    return;
  }

  worker_running_ = false;

  if (work_event_) {
    work_event_->Set();
  }

  if (paused_) {
    Resume();
  }

  // Wait up to 2 seconds for worker thread to exit gracefully.
  auto result = rex::thread::Wait(worker_thread_->thread(), false, std::chrono::milliseconds(2000));
  if (result == rex::thread::WaitResult::kTimeout) {
    REXAPU_WARN("XMA: Worker thread did not exit within 2s, abandoning");
  }
  worker_thread_.reset();

  if (context_data_first_ptr_) {
    memory()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
}

int XmaDecoder::GetContextId(uint32_t guest_ptr) {
  static_assert_size(XMA_CONTEXT_DATA, 64);
  if (guest_ptr < context_data_first_ptr_ || guest_ptr > context_data_last_ptr_) {
    return -1;
  }
  assert_zero(guest_ptr & 0x3F);
  return (guest_ptr - context_data_first_ptr_) >> 6;
}

uint32_t XmaDecoder::AllocateContext() {
  size_t index = context_bitmap_.Acquire();
  if (index == -1) {
    // Out of contexts.
    return 0;
  }

  XmaContext& context = contexts_[index];
  assert_false(context.is_allocated());
  context.set_is_allocated(true);
  return context.guest_ptr();
}

void XmaDecoder::ReleaseContext(uint32_t guest_ptr) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = contexts_[context_id];
  assert_true(context.is_allocated());
  context.Release();
  context_bitmap_.Release(context_id);
}

bool XmaDecoder::BlockOnContext(uint32_t guest_ptr, bool poll) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = contexts_[context_id];
  return context.Block(poll);
}

uint32_t XmaDecoder::ReadRegister(uint32_t addr) {
  auto r = (addr & 0xFFFF) / 4;

  assert_true(r < XmaRegisterFile::kRegisterCount);

  switch (r) {
    case XmaRegister::ContextArrayAddress:
      break;
    case XmaRegister::CurrentContextIndex: {
      // 0606h (1818h) is rotating context processing # set to hardware ID of
      // context being processed.
      // If bit 200h is set, the locking code will possibly collide on hardware
      // IDs and error out, so we should never set it (I think?).
      uint32_t& current_context_index = register_file_[XmaRegister::CurrentContextIndex];
      uint32_t& next_context_index = register_file_[XmaRegister::NextContextIndex];
      // To prevent games from seeing a stuck XMA context, return a rotating
      // number.
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXAPU_DEBUG("XMA: Read from unhandled register ({:04X}, {})", r, register_info->name);
      } else {
        REXAPU_DEBUG("XMA: Read from unknown register ({:04X})", r);
      }
      break;
  }

  return rex::byte_swap(register_file_[r]);
}

void XmaDecoder::WriteRegister(uint32_t addr, uint32_t value) {
  SCOPE_profile_cpu_f("apu");

  uint32_t r = (addr & 0xFFFF) / 4;
  value = rex::byte_swap(value);

  assert_true(r < XmaRegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XmaRegister::Context0Kick && r <= XmaRegister::Context9Kick) {
    // Context kick command.
    // This will kick off the given hardware contexts.
    // Basically, this kicks the SPU and says "hey, decode that audio!"
    // XMAEnableContext

    // The context ID is a bit in the range of the entire context array.
    uint32_t base_context_id = (r - XmaRegister::Context0Kick) * 32;
    uint32_t kicked_value = value;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Enable();
      }
    }
    /*
     * Who decodes (measured on the console).
     *
     * The thread requesting the kick used to decode right here, because waiting for the worker's sweep
     * stalled the audio for 50-100 ms. But on the console that thread is the game's audio server, it
     * runs at 0x2D (below the GPU ring, 0x2C) and the ring preempts it in the middle of decoding:
     * 1,555 kicks/s, 17.3 % of a core and peaks of 14.39 ms, when the buffer cushion is 10.7 ms.
     *
     * The worker runs at 0x2B, above the ring. When notified before anything is decoded, on the Switch
     * it starts immediately (it preempts the kick requester) and the game, which does not wait for
     * notifications but polls the context sleeping 50 us per iteration, finds the data already written.
     */
    const int32_t worker_mode = REXCVAR_GET(audio_xma_in_worker);
    const int64_t kick_before = NowNs();
    uint64_t kick_contexts = 0;
    if (worker_mode != 0) {
      uint32_t marked = kicked_value;
      for (int i = 0; marked && i < 32; ++i, marked >>= 1) {
        if (marked & 1) {
          MarkPending(base_context_id + i);
        }
      }
      g_pending_from_ns.store(NowNs(), std::memory_order_release);
      // blur-nx (2026-11-13, #65): a worker that is not waiting finds the marks itself (g_worker_waiting).
      static const bool idle_only = REXCVAR_GET(audio_xma_kick_wake_idle_only);
      if (!idle_only || g_worker_waiting.load(std::memory_order_seq_cst)) {
        work_event_->Set();
      } else {
        g_kick_wakes_skipped.fetch_add(1, std::memory_order_relaxed);
      }
    }
    for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
      if (kicked_value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        // Mode 1: the worker does everything. Mode 2: only what the worker has not already taken is
        // finished here, without waiting for it (useful on the PC, where it has priority over nobody).
        if (worker_mode == 1) {
          continue;
        }
        if (worker_mode == 2 && !TakePending(context_id)) {
          continue;  // the worker has taken it
        }
        ++kick_contexts;
        if (worker_mode != 0) {
          g_measurement_xma.contexts_backup.fetch_add(1, std::memory_order_relaxed);
        }
        if (context.Work()) {
          context.SignalWorkDone();
        }
      }
    }
    // Measurement only: how much decoding right here costs the thread requesting the kick.
    const uint64_t kick_ns = FromNs(kick_before);
    g_measurement_xma.kicks.fetch_add(1, std::memory_order_relaxed);
    g_measurement_xma.kick_contexts.fetch_add(kick_contexts, std::memory_order_relaxed);
    g_measurement_xma.kick_ns.fetch_add(kick_ns, std::memory_order_relaxed);
    MaxXma(g_measurement_xma.kick_max_ns, kick_ns);
    if (worker_mode == 0) {
      // With the worker already notified above, repeating the notification leaves the (auto-reset) event
      // signaled after its iteration and costs a full sweep of the 320 contexts per kick, 1,555 times per
      // second.
      work_event_->Set();
    }
    ReportXma();
  } else if (r >= XmaRegister::Context0Lock && r <= XmaRegister::Context9Lock) {
    // Context lock command.
    // This requests a lock by flagging the context.
    // XMADisableContext
    uint32_t base_context_id = (r - XmaRegister::Context0Lock) * 32;
    const int64_t lock_before = NowNs();
    uint64_t lock_contexts = 0;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        ++lock_contexts;
        context.Disable();
        // [XMA fix] Added Block(false) after Disable(). Without this, the game
        // could call XMADisableContext and start modifying the context struct
        // while a decode was still in progress on the worker thread. Block()
        // waits for the context mutex to be free (poll=false means wait, not spin).
        context.Block(false);
      }
    }
    // Measurement only: how long the thread requesting the lock waits for the worker to release the
    // context.
    const uint64_t lock_ns = FromNs(lock_before);
    g_measurement_xma.locks.fetch_add(1, std::memory_order_relaxed);
    g_measurement_xma.lock_contexts.fetch_add(lock_contexts, std::memory_order_relaxed);
    g_measurement_xma.lock_ns.fetch_add(lock_ns, std::memory_order_relaxed);
    MaxXma(g_measurement_xma.lock_max_ns, lock_ns);
    ReportXma();
    // Signal the decoder thread to start processing.
    // work_event_->Set();
  } else if (r >= XmaRegister::Context0Clear && r <= XmaRegister::Context9Clear) {
    // Context clear command.
    // This will reset the given hardware contexts.
    uint32_t base_context_id = (r - XmaRegister::Context0Clear) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        XmaContext& context = contexts_[context_id];
        context.Clear();
      }
    }
  } else {
    // 0601h (1804h) is written to with 0x02000000 and 0x03000000 around a lock
    // operation
    switch (r) {
      default: {
        const auto register_info = register_file_.GetRegisterInfo(r);
        if (register_info) {
          REXAPU_DEBUG("XMA: Write to unhandled register ({:04X}, {}): {:08X}", r,
                       register_info->name, value);
        } else {
          REXAPU_DEBUG("XMA: Write to unknown register ({:04X}): {:08X}", r, value);
        }
        break;
      }
#pragma warning(suppress : 4065)
    }
  }
}

void XmaDecoder::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  if (work_event_) {
    work_event_->Set();
  }
  pause_fence_.Wait();
}

void XmaDecoder::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_fence_.Signal();
}

}  // namespace rex::audio
