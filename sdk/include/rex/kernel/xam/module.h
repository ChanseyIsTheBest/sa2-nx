#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <string>

#include <rex/ppc/function.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

class XamModule : public system::KernelModule {
 public:
  XamModule(Runtime* emulator, system::KernelState* kernel_state);
  virtual ~XamModule();

  static void RegisterExportTable(rex::runtime::ExportResolver* export_resolver);

  struct LoaderData {
    bool launch_data_present = false;
    std::vector<uint8_t> launch_data;
    uint32_t launch_flags = 0;
    std::string launch_path;  // Full path to next xex
  };

  const LoaderData& loader_data() const { return loader_data_; }
  LoaderData& loader_data() { return loader_data_; }

 private:
  LoaderData loader_data_;
};

// ds3-nx (2026-10-24, sdk-patches/0014; port 57): a multi-disc title's own XamSwapDisc (xam_info.cpp): the handler
// gets the guest call as it is (r3 the disc number, r4 the completion event) and answers in r3. nullptr: the stub
// (success, nothing switched). Set it before the guest starts.
void SetSwapDiscHandler(void (*handler)(PPCContext& ctx, uint8_t* base));

}  // namespace xam
}  // namespace kernel
}  // namespace rex
