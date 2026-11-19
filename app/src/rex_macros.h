// renut - small helpers for hooks that run without a ctx/base pair.
//
// This header was referenced by the original reNut sources but was not part of
// the published repository; it is reconstructed from how the hooks use it.
#pragma once

#include <cstdint>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

// Declares `name` as the host pointer to guest virtual address 0. Midasm hooks
// receive only the registers they ask for, so they cannot use the `base`
// argument that full function hooks get. Do not hard-code 0x100000000: the
// guest arena lives elsewhere on other hosts (the Switch maps it at runtime).
#define REX_PPC_MEMBASE_PTR(name) \
  uint8_t* const name = ::rex::system::kernel_state()->memory()->virtual_membase()
