// =============================================================================
// Switch build: no keyboard or mouse.
//
// The codegen config (renut_hooks.toml) inserts three midasm hooks for the
// keyboard+mouse path, so the recompiled code references them on every
// platform. Here they do nothing: the pad state the game builds is left as the
// libnx controllers produced it. mnk_controls.cpp (Windows window events, the
// XInputGetKeystroke override) is not compiled on the Switch.
// =============================================================================

#include <rex/ppc.h>

#include "renut_engine/mnk_controls.h"

void renutMnk_PadConnectMask() {}
void renutMnk_PadPollResult(PPCRegister&, PPCRegister&, PPCRegister&) {}
void renutMnk_ApplyPadState(PPCRegister&, PPCRegister&) {}

namespace renut::mnk {

void AttachWindow(rex::ui::Window*) {}
void DetachWindow() {}
bool Active() { return false; }
void SetOverlayOpen(bool) {}
void BeginCapture() {}
void CancelCapture() {}
bool IsCapturing() { return false; }
bool TakeCaptured(std::string&) { return false; }

const BindInfo* BindList(std::size_t& out_count) {
  out_count = 0;
  return nullptr;
}

}  // namespace renut::mnk
