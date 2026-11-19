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

#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/stream.h>
#include <rex/system/thread_state.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/wait_registry.h>
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/sys_counters.h>

#include "global_lock_spin.h"

// Port 166 (from fnxf_xiii): a wait with an absolute timeout (a guest system time) waits until that time; it timed out
// at once before (0 ms). A fix, so off by default here.
REXCVAR_DEFINE_BOOL(banjo_wait_absolute_timeouts, false, "Kernel",
                    "A guest wait with an absolute timeout waits until that time by the guest clock (off: it times out "
                    "at once, as before)");

// Port 167 (from fnxf_xiii, its notes of 2026-10-22 and 10-25): wait_registry.h WaitGuestCallers.
void rex::system::waitreg::WaitGuestCallers(uint32_t out[5]) {
  for (int i = 0; i < 5; ++i)
    out[i] = 0;
  auto* thread = rex::system::XThread::GetCurrentThread();
  auto* ts = rex::runtime::ThreadState::Get();
  if (!thread || !ts || !ts->context() || !thread->is_guest_thread())
    return;
  out[0] = uint32_t(ts->context()->lr);
  // The guest stack's back chain: each frame's word 0 is the caller's r1, and a function that calls saves its LR at
  // -8 from that. Only inside the thread's own stack (KTHREAD stack_limit..stack_base): the outermost frame's back
  // chain points at the stack's top, past which nothing is mapped (fnxf_xiii read guest 0x70300000 there and faulted).
  auto* kt = thread->guest_object<rex::system::X_KTHREAD>();
  const uint32_t stack_high = kt ? uint32_t(kt->stack_base) : 0, stack_low = kt ? uint32_t(kt->stack_limit) : 0;
  auto in_stack = [&](uint32_t a) { return stack_low && a >= stack_low && a + 4 <= stack_high; };
  auto* mem = rex::system::kernel_state()->memory();
  uint32_t frame = ts->context()->r1.u32;
  for (int i = 1; i < 5 && in_stack(frame); ++i) {
    const uint32_t prev = rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual(frame));
    if (prev <= frame || !in_stack(prev - 8))
      break;
    out[i] = rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual(prev - 8));
    frame = prev;
  }
}

namespace rex::system {

void ApplyGuestCoreLayoutAtWait();  // dishonored-nx (sa2-nx's; port 103): xthread.cpp, RexSetGuestThreadCoreLayout

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    if (REXCVAR_GET(banjo_wait_absolute_timeouts)) {
      // Port 166 (from fnxf_xiii): the time left until then by the guest system clock (0 when it has passed).
      const int64_t now = static_cast<int64_t>(rex::chrono::Clock::QueryGuestSystemTime());
      if (timeout_ticks <= now)
        return 0;
      const uint64_t ms = static_cast<uint64_t>(timeout_ticks - now) / 10000;
      return ms >= 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<uint32_t>(ms);
    }
    // TODO(benvanik): convert time to relative time.
    assert_always();
    return 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  ApplyGuestCoreLayoutAtWait();  // dishonored-nx (sa2-nx's; port 103)
  REX_SYS_CENSUS(rex::syscount::kCensusWait, 1);
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  // blur-nx (2026-11-14, rex/system/wait_registry.h): the stall dumps' record of a blocked wait (polls are not marked).
  waitreg::Begin(handles_.empty() ? 0u : uint32_t(handles_[0]), uint32_t(type_), 1, waitreg::kWaitOne,
                 opt_timeout ? int64_t(timeout_ms.count()) : -1);
  auto result = rex::thread::Wait(wait_handle, alertable ? true : false, timeout_ms);
  waitreg::End();
  XThread::CheckTitleTermination();
  // blur-nx (2026-11-13, #57 / #34's census, PC): polls, and their timeouts.
  const bool poll = timeout_ms.count() == 0;
  if (poll) REX_SYS_CENSUS(rex::syscount::kCensusWaitPoll, 1);
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      REX_SYS_CENSUS(rex::syscount::kCensusWaitSignalled, 1);
      WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      if (poll) REX_SYS_CENSUS(rex::syscount::kCensusWaitPollTimeout, 1);
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  ApplyGuestCoreLayoutAtWait();  // dishonored-nx (sa2-nx's; port 103)
  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  // blur-nx (2026-11-14, rex/system/wait_registry.h): the stall dumps' record of a blocked wait (polls are not marked).
  waitreg::NoteSignalOf(signal_object,
                        signal_object->type() == Type::Event ? uint32_t(waitreg::kSet) : uint32_t(waitreg::kRelease));
  waitreg::Begin(wait_object->handles_.empty() ? 0u : uint32_t(wait_object->handles_[0]), uint32_t(wait_object->type_), 1,
                 waitreg::kSignalAndWait, opt_timeout ? int64_t(timeout_ms.count()) : -1);
  auto result =
      rex::thread::SignalAndWait(signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
                                 alertable ? true : false, timeout_ms);
  waitreg::End();
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      wait_object->WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  ApplyGuestCoreLayoutAtWait();  // dishonored-nx (sa2-nx's; port 103)
  // port 59 (2026-11-10, from LostOdysseyRecomp 299ac550): no heap allocation per call: up to 64 objects
  // (the 360's MAXIMUM_WAIT_OBJECTS) on the stack, more on the heap as before (newlib's malloc has one lock
  // for every thread).
  rex::thread::WaitHandle* inline_handles[64];
  std::vector<rex::thread::WaitHandle*> heap_handles;
  rex::thread::WaitHandle** wait_handles = inline_handles;
  if (count > 64) {
    heap_handles.resize(count);
    wait_handles = heap_handles.data();
  }
  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  // blur-nx (2026-11-14, rex/system/wait_registry.h): the stall dumps' record of a blocked wait (polls are not marked).
  waitreg::Begin(count == 0 || objects[0]->handles_.empty() ? 0u : uint32_t(objects[0]->handles_[0]),
                 count == 0 ? 0u : uint32_t(objects[0]->type_), count, wait_type ? waitreg::kWaitAny : waitreg::kWaitAll,
                 opt_timeout ? int64_t(timeout_ms.count()) : -1);
  if (wait_type) {
    auto result =
        rex::thread::WaitAny(wait_handles, count, alertable ? true : false, timeout_ms);
    waitreg::End();
    XThread::CheckTitleTermination();
    switch (result.first) {
      case rex::thread::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    auto result =
        rex::thread::WaitAll(wait_handles, count, alertable ? true : false, timeout_ms);
    waitreg::End();
    XThread::CheckTitleTermination();
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);
  REX_SYS_CENSUS(rex::syscount::kCensusNativeObject, 1);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto global_lock = AcquireGlobalSpin(rex::thread::global_critical_region::mutex());

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);

  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list_flink == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list_blink;
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    StashHandle(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
