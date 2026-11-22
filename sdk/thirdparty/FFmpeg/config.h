#if defined(WIN32) || defined(_WIN32)
#if defined(__aarch64__) || defined(_M_ARM64)
#include "config_windows_aarch64.h"
#else
#include "config_windows_x86_64.h"
#endif
#elif defined(__APPLE__) && defined(__MACH__)
#if defined(__aarch64__) || defined(__arm64__)
#include "config_macos_aarch64.h"
#else
#include "config_macos_x86_64.h"
#endif
#elif defined(__SWITCH__)
/* Nintendo Switch (devkitA64 + libnx), as in nfsmw-nx. */
#include "config_switch_aarch64.h"
#elif defined(__gnu_linux__)
#if defined(__aarch64__)
#include "config_linux_aarch64.h"
#else
#include "config_linux_x86_64.h"
#endif
#elif defined(__ANDROID__)
#if defined(__aarch64__)
#include "config_android_aarch64.h"
#elif defined(__amd64__) || defined(__x86_64__)
#include "config_android_x86_64.h"
#else
#error "android arch unsupported"
#endif
#else
#error "no config"
#endif
/* sa2-nx-ult (2026-11-22): the fork addition config_masseffect_wmv3.h says config.h includes it after each platform's
 * header, and its sources are built (thirdparty/CMakeLists.txt), but this file did not: CONFIG_WMV3_DECODER stayed 0
 * and FFmpeg had no WMV3 decoder (src/sa2_interviews.cpp plays the original interviews with it). */
#include "config_masseffect_wmv3.h"
