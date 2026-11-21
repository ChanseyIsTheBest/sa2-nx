// rr6-nx tools/test_lz4_blocks.cpp: the SDK's settings as plain bools a test sets from the environment
// (LZ4B_lz4b_load_log=1 turns the load log on).
#pragma once
#include <cstdlib>
#include <cstring>

namespace rex::cvar {
enum class Lifecycle { kInitOnly, kRequiresRestart };
struct StubBool {
  bool value;
  StubBool &lifecycle(Lifecycle) { return *this; }
};
inline bool FromEnv(const char *name, bool fallback) {
  const char *v = std::getenv(name);
  return v ? std::strcmp(v, "0") != 0 : fallback;
}
}  // namespace rex::cvar

#define REXCVAR_DEFINE_BOOL(name, def, cat, desc) \
  static rex::cvar::StubBool cvar_##name = rex::cvar::StubBool{rex::cvar::FromEnv("LZ4B_" #name, def)}
#define REXCVAR_GET(name) (cvar_##name.value)
