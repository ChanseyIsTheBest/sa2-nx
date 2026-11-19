/**
 * renut-ultimate (2026-11-16, port 147; tw2de_nx's xboxkrnl_timezone.cpp of 2026-11-03, as it was): the console's time
 * zone for XConfig's TIME_ZONE settings (xboxkrnl_xconfig.cpp, xconfig_host_time_zone, off by default).
 *
 * The 360 keeps the system time in UTC (KeQuerySystemTime) and the time zone in XConfig; a game's CRT turns one into
 * local time with the other (GetTimeZoneInformation reads XCONFIG_USER_TIME_ZONE_*). The SDK answered 0 for all of
 * them, so every game showed UTC. On the Switch the zone is the console's own (System Settings > Time Zone): its
 * offset from UTC now, DST included, as a Windows-style bias in minutes (UTC = local + bias). Kept apart from the
 * kernel file so that <switch.h> (its u32 / s32 types) does not meet the SDK's.
 */
#include <cstdint>
#include <ctime>

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

namespace rex::kernel::xboxkrnl {

// The current offset of local time from UTC, as a bias in minutes (UTC = local + bias): -60 for UTC+1. 0 when the
// console does not say (and on other platforms, as before).
int32_t HostTimeZoneBiasMinutes() {
#if REX_PLATFORM_SWITCH
  u64 now = 0;
  if (R_FAILED(timeGetCurrentTime(TimeType_Default, &now)))
    now = static_cast<u64>(std::time(nullptr));
  TimeCalendarTime cal{};
  TimeCalendarAdditionalInfo info{};
  if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &cal, &info)))
    return -(info.offset / 60);
#endif
  return 0;
}

}  // namespace rex::kernel::xboxkrnl
