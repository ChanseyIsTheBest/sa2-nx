// banjo-nx - RenderDoc frame capture on demand (PC diagnostic, banjo_renderdoc_capture_s).
//
// The SDK looks for RenderDoc before vkCreateInstance, when a layer enabled through VK_INSTANCE_LAYERS has not loaded
// renderdoc.dll yet; by the time a capture is wanted the layer is in the process, so its in-application API is taken
// from the loaded module here.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(banjo_renderdoc_game_s, 0, "Nuts&Bolts/Graphics",
                     "Diagnostic: this many seconds after the first game frame, ask RenderDoc to capture what the GPU "
                     "is given over the next 3 game frames into capturas/rdc (any renderer, emulated included; 0 = off)");

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <renderdoc_app.h>
#endif

namespace banjo::renderdoc {

#if defined(_WIN32)
static RENDERDOC_API_1_0_0* Api() {
  HMODULE module = GetModuleHandleA("renderdoc.dll");
  if (!module) return nullptr;
  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
  RENDERDOC_API_1_0_0* api = nullptr;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api))) return nullptr;
  return api;
}
#endif

// Starts capturing everything submitted from now on (any device, any window): the native renderer ends it at the
// next guest Swap, so the capture holds exactly one game frame even with the window minimized (no present needed).
bool Start(const char* path_template) {
#if defined(_WIN32)
  RENDERDOC_API_1_0_0* api = Api();
  if (!api) return false;
  api->SetLogFilePathTemplate(path_template);
  api->StartFrameCapture(nullptr, nullptr);
  return true;
#else
  (void)path_template;
  return false;
#endif
}

bool End() {
#if defined(_WIN32)
  RENDERDOC_API_1_0_0* api = Api();
  return api && api->EndFrameCapture(nullptr, nullptr);
#else
  return false;
#endif
}

// Called once per game frame (appMainDraw): drives banjo_renderdoc_game_s. Unlike banjo_renderdoc_capture_s (native
// renderer swaps) this does not know where the renderer's frames start, so it keeps a few game frames of GPU work.
void GameFrame() {
#if defined(_WIN32)
  static const int32_t at = REXCVAR_GET(banjo_renderdoc_game_s);
  if (at <= 0) return;
  static const auto start = std::chrono::steady_clock::now();
  static int phase = 0;  // 0 waiting, 1..3 capturing, 4 done
  if (phase == 0) {
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(at)) return;
    const auto folder = rex::filesystem::GetExecutableFolder() / "capturas" / "rdc";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (!Start((folder / "banjo").string().c_str())) {
      REXLOG_WARN("[renderdoc] game-frame capture requested but RenderDoc is not loaded");
      phase = 4;
      return;
    }
    REXLOG_INFO("[renderdoc] game-frame capture started");
    phase = 1;
  } else if (phase < 4 && ++phase == 4) {
    REXLOG_INFO("[renderdoc] game-frame capture ended: {}", End() ? "ok" : "FAILED");
  }
#endif
}

}  // namespace banjo::renderdoc
