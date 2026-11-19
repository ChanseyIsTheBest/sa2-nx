// katamari-nx (2026-11-01): diagnostic, off by default. Logs the fields of the game's global state object (the
// pointer at 0x82B0B568) that look like the stage and mission selection, each time one of them changes, so a test
// run through the menus into a level shows which one says what stage is loaded. Purpose: loading every stage in an
// emulator run to capture the shaders the game compiles at stage load (docs/katamari-notes.md, "rc8").
//
// Read from a host thread every 50 ms; the object is written by the game before the pointer is set, so its pages are
// committed by then.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(__SWITCH__)
#include <switch.h>  // std::thread is not implemented by the Switch's C++ library (std::system_error at start)
#endif

REXCVAR_DEFINE_BOOL(katamari_diag_game_state, false, "Katamari/Debug",
                    "Diagnostic: log the stage / mission fields of the game's state object whenever one changes "
                    "(\"[stage]\" lines; test runs only)");

namespace katamari::stage_diag {
namespace {

constexpr uint32_t kStatePointer = 0x82B0B568;
// halfwords, then bytes, then words (offsets into the state object)
constexpr uint16_t kHalves[] = {788, 790, 792, 964, 966, 968, 970, 972, 974, 976};
constexpr uint16_t kBytes[] = {978, 979, 1024, 1025, 1026, 1028, 1030, 1031, 1033, 1036, 1039, 1040, 1043,
                               1044, 1045, 1046, 1047, 1048, 1051, 1052, 1053, 1060, 1068, 1069, 1072, 1075, 1076};
constexpr uint16_t kWords[] = {800, 804, 836, 944, 956, 960, 1012, 1016, 1080};

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

void Run() {
  auto* memory = rex::system::kernel_state()->memory();
  std::string last;
  uint32_t lines = 0;
  const auto start = std::chrono::steady_clock::now();
  while (lines < 4000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint32_t object = Be32(memory->TranslateVirtual(kStatePointer));
    if (!object) continue;
    const uint8_t* p = memory->TranslateVirtual(object);
    std::string now = fmt::format("object {:08X} |", object);
    for (uint16_t o : kHalves) now += fmt::format(" h{}={}", o, Be16(p + o));
    now += " |";
    for (uint16_t o : kBytes) now += fmt::format(" b{}={}", o, p[o]);
    now += " |";
    for (uint16_t o : kWords) now += fmt::format(" w{}={:X}", o, Be32(p + o));
    if (now == last) continue;
    last = now;
    ++lines;
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REXLOG_INFO("[stage] {:.2f} s: {}", s, now);
  }
}

}  // namespace

void Start() {
  if (!REXCVAR_GET(katamari_diag_game_state) || !rex::system::kernel_state()) return;
#if defined(__SWITCH__)
  static Thread thread;
  if (R_SUCCEEDED(threadCreate(&thread, [](void*) { Run(); }, nullptr, nullptr, 0x10000, 0x3B, -2)))
    threadStart(&thread);
#else
  std::thread(Run).detach();
#endif
}

}  // namespace katamari::stage_diag
