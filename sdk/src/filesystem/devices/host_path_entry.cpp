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

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>
#include "startup_trace.h"

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/lz4_blocks.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/mapped_memory.h>
#include <rex/string.h>

namespace rex::filesystem {

HostPathEntry::HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                             const std::filesystem::path& host_path)
    : Entry(device, parent, path), host_path_(host_path) {}

HostPathEntry::~HostPathEntry() = default;

HostPathEntry* HostPathEntry::Create(Device* device, Entry* parent,
                                     const std::filesystem::path& full_path,
                                     rex::filesystem::FileInfo file_info) {
  std::string name = rex::path_to_utf8(file_info.name);
  /*
   * rr6-nx (2026-11-10): NAME.lz4b is the game's NAME (rex/filesystem/lz4_blocks.h), with the size of the file it
   * holds, unless a plain NAME lies next to it (an install extracted over an older one): the plain file wins and the
   * .lz4b stays an ordinary file the game never asks for.
   */
  bool lz4_blocks = false;
  uint64_t lz4_size = 0;
  if (file_info.type == rex::filesystem::FileInfo::Type::kFile && HasLz4BlocksSuffix(name)) {
    std::filesystem::path plain = full_path;
    plain.replace_extension();
    rex::filesystem::FileInfo plain_info;
    if (rex::filesystem::GetInfo(plain, &plain_info)) {
      REXFS_WARN("{} and {} are both in the game folder: the plain file is used (delete one of them)",
                 rex::path_to_utf8(plain), rex::path_to_utf8(full_path));
    } else if (ReadLz4BlocksSize(full_path, &lz4_size)) {
      lz4_blocks = true;
      name.resize(name.size() - kLz4BlocksSuffix.size());
    } else {
      REXFS_WARN("{} is not a valid .lz4b file: left as it is", rex::path_to_utf8(full_path));
    }
  }
  auto path = rex::string::utf8_join_guest_paths(parent->path(), name);
  auto entry = new HostPathEntry(device, parent, path, full_path);
  entry->lz4_blocks_ = lz4_blocks;
  if (lz4_blocks) {
    file_info.total_size = size_t(lz4_size);
  }

  entry->create_timestamp_ = file_info.create_timestamp;
  entry->access_timestamp_ = file_info.access_timestamp;
  entry->write_timestamp_ = file_info.write_timestamp;
  if (file_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
    entry->attributes_ = kFileAttributeDirectory;
  } else {
    entry->attributes_ = kFileAttributeNormal;
    if (device->is_read_only()) {
      entry->attributes_ |= kFileAttributeReadOnly;
    }
    entry->size_ = file_info.total_size;
    entry->allocation_size_ = rex::round_up(file_info.total_size, device->bytes_per_sector());
  }
  return entry;
}

X_STATUS HostPathEntry::Open(uint32_t desired_access, File** out_file) {
  if (is_read_only() &&
      (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    REXFS_ERROR("Attempting to open file for write access on read-only device");
    return X_STATUS_ACCESS_DENIED;
  }
  /*
   * A folder is not opened as a file.
   *
   * On the Xbox 360, opening a directory is legal: games do it to enumerate it or just to check that
   * the drive is mounted. The game opens the root of its save container ("rmcsave:\") right after
   * mounting it, and this code used to try to open a FILE* on that folder. That always fails,
   * NO_SUCH_FILE was returned, and the game read it as "this save cannot be used": hence the
   * "damaged profile" on load. Creating a profile did not hit it, because then the game writes a
   * real file inside.
   *
   * A File without a handle is enough: what is done with a directory is enumerating it, and
   * XFile::QueryDirectory walks the entry tree without touching the disk. Reads and writes on it
   * are rejected further down.
   */
  if (attributes_ & kFileAttributeDirectory) {
    *out_file = new HostPathFile(desired_access, this, nullptr);
    return X_STATUS_SUCCESS;
  }

  // rr6-nx (2026-11-10): an .lz4b file is read only.
  if (lz4_blocks_ && (desired_access & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                                        FileAccess::kFileAppendData | FileAccess::kGenericAll))) {
    return X_STATUS_ACCESS_DENIED;
  }

  // A read handle the startup preload thread opened ahead of the guest (read-only access only).
  std::unique_ptr<rex::filesystem::FileHandle> file_handle;
  if (is_read_only() && !(desired_access & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                                            FileAccess::kFileAppendData | FileAccess::kGenericAll))) {
    file_handle = startup_trace::TakePreopened(static_cast<HostPathDevice*>(device_), path());
  }
  if (!file_handle) {
    file_handle = rex::filesystem::FileHandle::OpenExisting(
        host_path_, desired_access, static_cast<HostPathDevice*>(device_)->allow_share_delete());
  }
  if (!file_handle) {
    // TODO(benvanik): pick correct response.
    return X_STATUS_NO_SUCH_FILE;
  }
  if (lz4_blocks_) {
    file_handle = OpenLz4Blocks(std::move(file_handle));  // logs why when it is not a valid .lz4b file
    if (!file_handle) {
      return X_STATUS_NO_SUCH_FILE;
    }
  }
  *out_file = new HostPathFile(desired_access, this, std::move(file_handle));
  return X_STATUS_SUCCESS;
}

std::unique_ptr<memory::MappedMemory> HostPathEntry::OpenMapped(memory::MappedMemory::Mode mode,
                                                                size_t offset, size_t length) {
  if (lz4_blocks_) {
    return nullptr;  // rr6-nx: the host file is the packed one
  }
  return memory::MappedMemory::Open(host_path_, mode, offset, length);
}

bool HostPathEntry::Truncate() {
  if (is_read_only() || (attributes_ & kFileAttributeDirectory)) {
    return false;
  }
  auto file = rex::filesystem::OpenFile(host_path_, "wb");
  if (!file) {
    return false;
  }
  fclose(file);
  size_ = 0;
  allocation_size_ = 0;
  return true;
}

std::unique_ptr<Entry> HostPathEntry::CreateEntryInternal(const std::string_view name,
                                                          uint32_t attributes) {
  auto full_path = host_path_ / rex::to_path(name);
  if (attributes & kFileAttributeDirectory) {
    if (!std::filesystem::create_directories(full_path)) {
      return nullptr;
    }
  } else {
    auto file = rex::filesystem::OpenFile(full_path, "wb");
    if (!file) {
      return nullptr;
    }
    fclose(file);
  }
  rex::filesystem::FileInfo file_info;
  if (!rex::filesystem::GetInfo(full_path, &file_info)) {
    return nullptr;
  }
  return std::unique_ptr<Entry>(HostPathEntry::Create(device_, this, full_path, file_info));
}

bool HostPathEntry::DeleteEntryInternal(Entry* entry) {
  auto full_path = host_path_ / rex::to_path(entry->name());
  std::error_code ec;  // avoid exception on remove/remove_all failure
  if (entry->attributes() & kFileAttributeDirectory) {
    // Delete entire directory and contents.
    auto removed = std::filesystem::remove_all(full_path, ec);
    return removed >= 1 && removed != static_cast<std::uintmax_t>(-1);
  } else {
    // Delete file.
    return !std::filesystem::is_directory(full_path) && std::filesystem::remove(full_path, ec);
  }
}

X_STATUS HostPathEntry::RenameEntryInternal(const std::vector<std::string_view>& path_parts) {
  auto new_host_path = static_cast<HostPathDevice*>(device_)->host_path();
  for (const auto& path_part : path_parts) {
    new_host_path /= rex::to_path(path_part);
  }

  std::error_code ec;
  std::filesystem::rename(host_path_, new_host_path, ec);
  if (ec) {
    REXFS_ERROR("RenameEntryInternal: failed to rename '{}' to '{}': {} ({})",
                rex::path_to_utf8(host_path_), rex::path_to_utf8(new_host_path), ec.message(),
                ec.value());
    return X_STATUS_ACCESS_DENIED;
  }

  host_path_ = new_host_path;
  return X_STATUS_SUCCESS;
}

void HostPathEntry::update() {
  rex::filesystem::FileInfo file_info;
  if (!rex::filesystem::GetInfo(host_path_, &file_info)) {
    return;
  }
  if (file_info.type == rex::filesystem::FileInfo::Type::kFile) {
    if (lz4_blocks_) {  // rr6-nx: the size of the file it holds
      uint64_t size = 0;
      if (!ReadLz4BlocksSize(host_path_, &size)) {
        return;
      }
      file_info.total_size = size_t(size);
    }
    size_ = file_info.total_size;
    allocation_size_ = rex::round_up(file_info.total_size, device()->bytes_per_sector());
  }
}

bool HostPathEntry::SetAttributes(uint64_t attributes) {
  if (device_->is_read_only()) {
    return false;
  }
  attributes_ = static_cast<uint32_t>(attributes);
  return true;
}

bool HostPathEntry::SetCreateTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    return false;
  }
  create_timestamp_ = timestamp;
  return true;
}

bool HostPathEntry::SetAccessTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    return false;
  }
  access_timestamp_ = timestamp;
  return true;
}

bool HostPathEntry::SetWriteTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    return false;
  }
  write_timestamp_ = timestamp;
  return true;
}

}  // namespace rex::filesystem
