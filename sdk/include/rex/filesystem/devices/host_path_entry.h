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

#pragma once

#include <string>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>

namespace rex::filesystem {

class HostPathDevice;

class HostPathEntry : public Entry {
 public:
  HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                const std::filesystem::path& host_path);
  ~HostPathEntry() override;

  static HostPathEntry* Create(Device* device, Entry* parent,
                               const std::filesystem::path& full_path,
                               rex::filesystem::FileInfo file_info);

  const std::filesystem::path& host_path() const { return host_path_; }
  // rr6-nx (2026-11-10): host_path_ is an LZ4 block file (NAME.lz4b, rex/filesystem/lz4_blocks.h) shown as NAME with
  // the size of the file it holds; opened through OpenLz4Blocks, never mapped or written.
  bool lz4_blocks() const { return lz4_blocks_; }

  /*
   * "All the children of this directory are already known."
   *
   * HostPathDevice::Initialize walks the game's whole tree and creates an entry for every file. Once
   * that has happened, GetChild failing is the final answer: the file does not exist. Without this flag
   * the device went to the SD to check again (a stat and, if that failed, a full enumeration of the
   * directory with one stat per entry), which cost ~32 ms per failed open. See host_path_device.cpp.
   */
  bool children_complete() const { return children_complete_; }

  // Only for rebuilding the tree from the on-disk index (host_path_index.cpp).
  void AddChildIndex(HostPathEntry* child) { children_.push_back(std::unique_ptr<Entry>(child)); }
  void MarkCompleteIndex(bool complete) { children_complete_ = complete; }
  void DiscardChildrenIndex() {
    children_.clear();
    children_complete_ = false;
  }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;
  bool Truncate() override;

  bool can_map() const override { return !lz4_blocks_; }
  std::unique_ptr<memory::MappedMemory> OpenMapped(memory::MappedMemory::Mode mode, size_t offset,
                                                   size_t length) override;
  void update() override;
  bool SetAttributes(uint64_t attributes) override;
  bool SetCreateTimestamp(uint64_t timestamp) override;
  bool SetAccessTimestamp(uint64_t timestamp) override;
  bool SetWriteTimestamp(uint64_t timestamp) override;

 private:
  friend class HostPathDevice;

  std::unique_ptr<Entry> CreateEntryInternal(const std::string_view name,
                                             uint32_t attributes) override;
  bool DeleteEntryInternal(Entry* entry) override;
  X_STATUS RenameEntryInternal(const std::vector<std::string_view>& path_parts) override;

  std::filesystem::path host_path_;
  bool children_complete_ = false;
  bool lz4_blocks_ = false;
};

}  // namespace rex::filesystem
