// renut-ultimate (2026-11-20, port 193): TFU-nx's tfu_build_stamp.cpp (via S4ep1-nx). app/cmake/build_stamp.cmake
// writes build_stamp.h on every build. The Switch crash hooks print it first in rex_stderr.log, rex_profile.log and
// every crash report; the app logs it at start (renut_app.h).
#include "build_stamp.h"

extern "C" const char* RexAppBuildStamp(void) {
  return RENUT_BUILD_STAMP;
}
