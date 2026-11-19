/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/sys_counters.h>

#include "../global_lock_spin.h"

REXCVAR_DEFINE_INT32(global_lock_spin, 48, "Kernel",
                     "Handle-table lookups and guest-object resolution try the global critical region this many "
                     "times (CPU yield between tries, ~1 us in total) before blocking in the kernel. 0 = block at "
                     "once (the original behaviour)");
REXCVAR_DEFINE_BOOL(kernel_handle_lookup_own_lock, true, "Kernel",
                    "blur-nx (#56, exact): a handle lookup takes the handle table's own lock, held only while an entry "
                    "is read or written, not the global critical region (Blur: 11,700 lookups a second, 218 of them "
                    "finding the global region taken). false: the global critical region, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(kernel_handle_lookup_spin, true, "Kernel",
                    "blur-nx (the Renut audit's #9, exact): a handle lookup finding the handle table's own lock taken "
                    "(kernel_handle_lookup_own_lock) tries it global_lock_spin times more, a CPU yield between tries, "
                    "before it blocks in the kernel, as the global region's lookups did. false: it blocks at once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::system::util {

ObjectTable::ObjectTable() {}

ObjectTable::~ObjectTable() {
  Reset();
}

void ObjectTable::Reset() {
  auto global_lock = global_critical_region_.Acquire();

  // blur-nx (2026-11-13, #56): the table taken out under table_mutex_, its objects released after (a release may
  // destroy an object, whose teardown may look a handle up).
  ObjectTableEntry* old_table;
  uint32_t old_capacity;
  {
    std::lock_guard<std::mutex> table_lock(table_mutex_);
    old_table = table_;
    old_capacity = table_capacity_;
    table_capacity_ = 0;
    last_free_entry_ = 0;
    table_ = nullptr;
  }

  // Release all objects.
  for (uint32_t n = 0; n < old_capacity; n++) {
    ObjectTableEntry& entry = old_table[n];
    if (entry.object) {
      entry.object->Release();
    }
  }
  free(old_table);
}

X_STATUS ObjectTable::FindFreeSlot(uint32_t* out_slot) {
  // Find a free slot.
  uint32_t slot = last_free_entry_;
  uint32_t scan_count = 0;
  while (scan_count < table_capacity_) {
    ObjectTableEntry& entry = table_[slot];
    if (!entry.object) {
      *out_slot = slot;
      return X_STATUS_SUCCESS;
    }
    scan_count++;
    slot = (slot + 1) % table_capacity_;
    if (slot == 0) {
      // Never allow 0 handles.
      scan_count++;
      slot++;
    }
  }

  // Table out of slots, expand.
  uint32_t new_table_capacity = std::max(16 * 1024u, table_capacity_ * 2);
  if (!Resize(new_table_capacity)) {
    return X_STATUS_NO_MEMORY;
  }

  // Never allow 0 handles.
  slot = ++last_free_entry_;
  *out_slot = slot;

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Resize(uint32_t new_capacity) {
  uint32_t new_size = new_capacity * sizeof(ObjectTableEntry);
  uint32_t old_size = table_capacity_ * sizeof(ObjectTableEntry);
  std::lock_guard<std::mutex> table_lock(table_mutex_);  // blur-nx (2026-11-13, #56): realloc may move the table
  auto new_table = reinterpret_cast<ObjectTableEntry*>(realloc(table_, new_size));
  if (!new_table) {
    return false;
  }

  // Zero out new entries.
  if (new_size > old_size) {
    std::memset(reinterpret_cast<uint8_t*>(new_table) + old_size, 0, new_size - old_size);
  }

  last_free_entry_ = table_capacity_;
  table_capacity_ = new_capacity;
  table_ = new_table;

  return true;
}

X_STATUS ObjectTable::AddHandle(XObject* object, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  uint32_t handle = 0;
  {
    auto global_lock = global_critical_region_.Acquire();

    // Find a free slot.
    uint32_t slot = 0;
    result = FindFreeSlot(&slot);

    // Stash.
    if (XSUCCEEDED(result)) {
      {
        std::lock_guard<std::mutex> table_lock(table_mutex_);  // blur-nx (2026-11-13, #56)
        ObjectTableEntry& entry = table_[slot];
        entry.object = object;
        entry.handle_ref_count = 1;
      }
      handle = XObject::kHandleBase + (slot << 2);
      object->handles().push_back(handle);

      // Retain so long as the object is in the table.
      object->Retain();

      REXSYS_NOISY_DEBUG("Added handle:{:08X} for {}", handle, typeid(*object).name());
    }
  }

  if (XSUCCEEDED(result)) {
    if (out_handle) {
      *out_handle = handle;
    }
  }

  return result;
}

X_STATUS ObjectTable::DuplicateHandle(X_HANDLE handle, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;
  handle = TranslateHandle(handle);

  XObject* object = LookupObject(handle, false);
  if (object) {
    result = AddHandle(object, out_handle);
    object->Release();  // Release the ref that LookupObject took
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

X_STATUS ObjectTable::RetainHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  entry->handle_ref_count++;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleaseHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (--entry->handle_ref_count == 0) {
    // No more references. Remove it from the table.
    return RemoveHandle(handle);
  }

  // FIXME: Return a status code telling the caller it wasn't released
  // (but not a failure code)
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::RemoveHandle(X_HANDLE handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  handle = TranslateHandle(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }

  // blur-nx (2026-11-13, #56): the global region taken before the entry is found (a resize between moved it).
  auto global_lock = global_critical_region_.Acquire();
  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (entry->object) {
    auto object = entry->object;
    {
      std::lock_guard<std::mutex> table_lock(table_mutex_);  // blur-nx (2026-11-13, #56)
      entry->object = nullptr;
    }
    assert_zero(entry->handle_ref_count);
    entry->handle_ref_count = 0;

    // Walk the object's handles and remove this one.
    auto handle_entry = std::find(object->handles().begin(), object->handles().end(), handle);
    if (handle_entry != object->handles().end()) {
      object->handles().erase(handle_entry);
    }

    REXSYS_NOISY_DEBUG("Removed handle:{:08X} for {}", handle, typeid(*object).name());

    // Remove object name from mapping to prevent naming collision.
    if (!object->name().empty()) {
      RemoveNameMapping(object->name());
    }
    // Release now that the object has been removed from the table.
    object->Release();
  }

  return X_STATUS_SUCCESS;
}

std::vector<object_ref<XObject>> ObjectTable::GetAllObjects() {
  auto lock = global_critical_region_.Acquire();
  std::vector<object_ref<XObject>> results;

  for (uint32_t slot = 0; slot < table_capacity_; slot++) {
    auto& entry = table_[slot];
    if (entry.object && std::find(results.begin(), results.end(), entry.object) == results.end()) {
      entry.object->Retain();
      results.push_back(object_ref<XObject>(entry.object));
    }
  }

  return results;
}

void ObjectTable::PurgeAllObjects() {
  auto lock = global_critical_region_.Acquire();
  // blur-nx (2026-11-13, #56): the entries cleared under table_mutex_, their objects released after.
  std::vector<XObject*> purged;
  {
    std::lock_guard<std::mutex> table_lock(table_mutex_);
    for (uint32_t slot = 0; slot < table_capacity_; slot++) {
      auto& entry = table_[slot];
      if (entry.object && !entry.object->is_host_object()) {
        entry.handle_ref_count = 0;
        purged.push_back(entry.object);
        entry.object = nullptr;
      }
    }
  }
  for (XObject* object : purged) {
    object->Release();
  }
}

ObjectTable::ObjectTableEntry* ObjectTable::LookupTable(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Lower 2 bits are ignored.
  uint32_t slot = GetHandleSlot(handle);
  if (slot < table_capacity_) {
    return &table_[slot];
  }

  return nullptr;
}

// Generic lookup
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle) {
  auto object = ObjectTable::LookupObject(handle, false);
  auto result = object_ref<XObject>(reinterpret_cast<XObject*>(object));
  return result;
}

XObject* ObjectTable::LookupObject(X_HANDLE handle, bool already_locked) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  XObject* object = nullptr;
  // blur-nx (2026-11-13, #56, kernel_handle_lookup_own_lock): the table's own lock (see table_mutex_); a lookup
  // finding it taken counts as kObjBlocked, as one finding the global region taken did.
  static const bool own_lock = REXCVAR_GET(kernel_handle_lookup_own_lock);
  std::unique_lock<std::recursive_mutex> region;
  std::unique_lock<std::mutex> table_lock;
  if (own_lock) {
    table_lock = std::unique_lock<std::mutex>(table_mutex_, std::try_to_lock);
    if (!table_lock.owns_lock()) {
      REX_SYS_COUNT(rex::syscount::kObjBlocked, 1);
      // blur-nx (2026-11-17, the Renut audit's #9, kernel_handle_lookup_spin): the bounded spin the global region's
      // lookups had (global_lock_spin tries, a CPU yield between them) before the kernel wait: the lock is held for
      // a few loads and a retain, and Horizon's mutex hands a released lock to the waiter it wakes (Blur's race: 296
      // of 14,193 lookups a second found it taken).
      static const bool spin = REXCVAR_GET(kernel_handle_lookup_spin);
      bool got = false;
      if (spin) {
        for (int32_t i = REXCVAR_GET(global_lock_spin); i > 0 && !got; --i) {
          CpuRelax();
          got = table_lock.try_lock();
        }
      }
      if (!got)
        table_lock.lock();
    }
  } else if (!already_locked) {
    region = AcquireGlobalSpin(global_critical_region_.mutex(), rex::syscount::kObjBlocked);
  }
  REX_SYS_COUNT(rex::syscount::kObjLookup, 1);

  // Lower 2 bits are ignored.
  uint32_t slot = GetHandleSlot(handle);

  // Verify slot.
  if (slot < table_capacity_) {
    ObjectTableEntry& entry = table_[slot];
    if (entry.object) {
      object = entry.object;
    }
  }

  // Retain the object pointer.
  if (object) {
    object->Retain();
  }

  return object;
}

void ObjectTable::GetObjectsByType(XObject::Type type, std::vector<object_ref<XObject>>* results) {
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t slot = 0; slot < table_capacity_; ++slot) {
    auto& entry = table_[slot];
    if (entry.object) {
      if (entry.object->type() == type) {
        entry.object->Retain();
        results->push_back(object_ref<XObject>(entry.object));
      }
    }
  }
}

X_HANDLE ObjectTable::TranslateHandle(X_HANDLE handle) {
  if (handle == 0xFFFFFFFF) {
    // CurrentProcess
    // assert_always();
    return 0;
  } else if (handle == 0xFFFFFFFE) {
    // CurrentThread
    return XThread::GetCurrentThreadHandle();
  } else {
    return handle;
  }
}

X_STATUS ObjectTable::AddNameMapping(const std::string_view name, X_HANDLE handle) {
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  if (name_table_.count(string::string_key_case(name))) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  name_table_.insert({string::string_key_case::create(name), handle});
  return X_STATUS_SUCCESS;
}

void ObjectTable::RemoveNameMapping(const std::string_view name) {
  // Names are case-insensitive.
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  auto it = name_table_.find(string::string_key_case(name));
  if (it != name_table_.end()) {
    name_table_.erase(it);
  }
}

X_STATUS ObjectTable::GetObjectByName(const std::string_view name, X_HANDLE* out_handle) {
  // Names are case-insensitive.
  // Look up handle under name lock only -- do NOT hold name_mutex_ while
  // acquiring global lock (RemoveHandle takes global -> name ordering).
  X_HANDLE handle;
  {
    std::lock_guard<std::mutex> name_lock(name_mutex_);
    auto it = name_table_.find(string::string_key_case(name));
    if (it == name_table_.end()) {
      *out_handle = X_INVALID_HANDLE_VALUE;
      return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    handle = it->second;
  }
  *out_handle = handle;

  // Retain under global lock via normal LookupObject path.
  // The handle may have been removed between releasing name_mutex_ and
  // acquiring global lock -- LookupObject returns nullptr in that case.
  auto obj = LookupObject(handle, false);
  if (obj) {
    obj->RetainHandle();
    obj->Release();
  }

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Save(stream::ByteStream* stream) {
  stream->Write<uint32_t>(table_capacity_);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }

  return true;
}

bool ObjectTable::Restore(stream::ByteStream* stream) {
  Resize(stream->Read<uint32_t>());
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    // entry.object = nullptr;
    entry.handle_ref_count = stream->Read<int32_t>();
  }

  return true;
}

X_STATUS ObjectTable::RestoreHandle(X_HANDLE handle, XObject* object) {
  uint32_t slot = GetHandleSlot(handle);
  assert_true(table_capacity_ > slot);

  if (table_capacity_ > slot) {
    {
      std::lock_guard<std::mutex> table_lock(table_mutex_);  // blur-nx (2026-11-13, #56)
      table_[slot].object = object;
    }
    object->Retain();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::system::util
