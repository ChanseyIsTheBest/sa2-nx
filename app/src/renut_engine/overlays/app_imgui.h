#pragma once
#include "imgui.h"

// renut-ultimate (port 66, 2026-11-10): whether this executable's Dear ImGui can take a dialog's widgets now.
//
// On the PC the app's dialogs (FPS, log, profiler, path wizard, keyboard and mouse) are drawn by the SDK's ImGuiDrawer
// (gpu/overlay/app_integration.cpp, RecordSdkMenus), but their ImGui calls go to this executable's own copy of Dear
// ImGui, whose only context is Renut's overlay's (gpu/overlay/overlay_ui.cpp). That context's font is set by its
// first NewFrame; before it, g.Font is null and ImGui::Begin reads g.Font->OwnerAtlas at address 8 (the PC crash of
// 2026-11-10: three of six starts, when the SDK menus' first UI-thread task ran before the overlay's first frame).
// With no context at all (the overlay could not start: no game window) every call would read through a null GImGui.
// On the Switch there is one Dear ImGui, the SDK's, in its frame here: this is always true there.
inline bool AppImGuiReady() {
  return ImGui::GetCurrentContext() != nullptr && ImGui::GetFrameCount() > 0;
}
