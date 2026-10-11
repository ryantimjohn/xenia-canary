/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
#define XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  void Clear();

  Device* RegisterDevice(std::unique_ptr<Device> device);
  bool UnregisterDevice(const std::string_view path);

  bool RegisterSymbolicLink(const std::string_view path,
                            const std::string_view target);
  bool UnregisterSymbolicLink(const std::string_view path);
  bool IsSymbolicLinkRegistered(const std::string_view path);
  bool FindSymbolicLink(const std::string_view path, std::string& target);

  // Resolves a guest path to the entry it names, or null when no device is
  // mounted under its root or the device holds no such entry. A path whose
  // device is not mounted is logged as an error, because for nearly every
  // caller that means the guest asked for something xenia should have been
  // able to give it. Pass false for log_missing_device where the miss is
  // expected and handled - a title probing update:\ before game:\, say - and
  // the resolve stays silent; nothing else about it changes.
  Entry* ResolvePath(const std::string_view path,
                     bool log_missing_device = true);

  Entry* CreatePath(const std::string_view path, uint32_t attributes);
  bool DeletePath(const std::string_view path);

  X_STATUS OpenFile(Entry* root_entry, const std::string_view path,
                    FileDisposition creation_disposition,
                    uint32_t desired_access, bool is_directory,
                    bool is_non_directory, File** out_file,
                    FileAction* out_action);

  static X_STATUS ExtractDeviceFile(Entry* entry,
                                    std::filesystem::path base_path,
                                    uint64_t& progress,
                                    bool extract_to_root = false);
  static X_STATUS ExtractDeviceFiles(Device* device,
                                     std::filesystem::path base_path,
                                     uint64_t& progress);

 private:
  xe::global_critical_region global_critical_region_;
  std::vector<std::unique_ptr<Device>> devices_;
  std::unordered_map<std::string, std::string> symlinks_;

  bool ResolveSymbolicLink(const std::string_view path, std::string& result);
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
