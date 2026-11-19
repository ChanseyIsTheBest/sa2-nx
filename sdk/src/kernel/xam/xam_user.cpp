/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/thread_state.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");
// Port 159 (from fnxf_xiii): XamUserWriteProfileSettings keeps the INT32, INT64, DATETIME, DOUBLE, FLOAT and WSTRING
// settings too (only BINARY and CONTENT were kept: a game's other saved options were lost, "Unimplemented data type").
REXCVAR_DEFINE_BOOL(banjo_xam_profile_all_types, false, "Kernel",
                    "XamUserWriteProfileSettings keeps the int, int64, date, double, float and string settings too "
                    "(off: only the binary ones, as before)");
// culdcept-nx (2026-11-19, the user: "I can't join multiplayer with a second controller"): what the game asks about users
// 1-3 and what it is told, logged when an answer changes (and every sign-in screen).
REXCVAR_DEFINE_BOOL(xam_user_trace, false, "Kernel",
                    "Diagnostic: the game's questions about users 1-3 (sign-in state and info, XUID, name, privileges) "
                    "and the answers, logged when an answer changes; every XamShowSigninUI");
// culdcept-nx (2026-11-19): Culdcept Saga's Versus lobby opens the sign-in screen and took no entry (not even player
// 1's) when no further pad was signed in: the screen "closed" (XN_SYS_UI off) without having "opened".
REXCVAR_DEFINE_BOOL(xam_signin_ui_opens, false, "Kernel",
                    "XamShowSigninUI sends XN_SYS_UI on before the sign-in change and XN_SYS_UI off, as the "
                    "console's sign-in screen opens and closes. false: only the change and the off (as before)");
REXCVAR_DEFINE_BOOL(xam_guest_signin, true, "Kernel",
                    "When the game opens the sign-in screen, every further connected controller (2 to 4) is signed "
                    "in as a guest of the profile, as choosing a guest on the console does. Off: only controller 1 "
                    "has a profile, and a game that wants one per local player takes no second player");
// blur-nx (2026-11-18, the user: "language (set it to system language by default)"): the player's setting by name. A
// game without the language shows its own fallback (Blur, like Beautiful Katamari, English), as on an Xbox 360 set to it.
REXCVAR_DEFINE_STRING(language, "System", "Kernel",
                      "The game's language: System (the console's own language), English, Japanese, German, French, "
                      "Spanish, Italian, Korean, Chinese (Traditional), Portuguese, Chinese (Simplified), Polish or "
                      "Russian; empty = user_language. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REX_PLATFORM_SWITCH
// libnx's set service (switch.h is not included here: its types clash with the SDK's).
extern "C" {
uint32_t setInitialize(void);
void setExit(void);
uint32_t setGetSystemLanguage(uint64_t* LanguageCode);
uint32_t setMakeLanguage(uint64_t LanguageCode, int* Language);
}
#endif

namespace rex::system {
uint32_t EffectiveLanguage() {
  static const uint32_t language = [] {
    // The Xbox 360's language IDs (XLanguage) by the names the key takes.
    static const std::pair<const char*, uint32_t> kNames[] = {
        {"English", 1},  {"Japanese", 2}, {"German", 3},  {"French", 4},
        {"Spanish", 5},  {"Italian", 6},  {"Korean", 7},  {"Chinese (Traditional)", 8},
        {"Portuguese", 9}, {"Chinese (Simplified)", 10}, {"Polish", 11}, {"Russian", 12},
    };
    const std::string& name = REXCVAR_GET(language);
    if (name.empty())
      return uint32_t(REXCVAR_GET(user_language));
    for (const auto& [n, id] : kNames)
      if (rex::string::compare_case(name.c_str(), n) == 0)
        return id;
    uint32_t id = 1;  // "System" (or an unknown name): the console's language, English off the Switch
#if REX_PLATFORM_SWITCH
    // SetLanguage (libnx's set.h) to XLanguage: the regional variants to their language; Dutch has none (English).
    static const uint32_t kFromSwitch[] = {
        2,   // JA
        1,   // ENUS
        4,   // FR
        3,   // DE
        6,   // IT
        5,   // ES
        10,  // ZHCN
        7,   // KO
        1,   // NL
        9,   // PT
        12,  // RU
        8,   // ZHTW
        1,   // ENGB
        4,   // FRCA
        5,   // ES419
        10,  // ZHHANS
        8,   // ZHHANT
        9,   // PTBR
    };
    if (setInitialize() == 0) {
      uint64_t code = 0;
      int lang = -1;
      if (setGetSystemLanguage(&code) == 0 && setMakeLanguage(code, &lang) == 0 && lang >= 0 &&
          lang < int(std::size(kFromSwitch)))
        id = kFromSwitch[lang];
      setExit();
    }
#endif
    REXLOG_INFO("language: {} (the key says \"{}\")", id, name);
    return id;
  }();
  return language;
}
}  // namespace rex::system

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {

// Guests of the profile: the users 1 to 3 the sign-in screen has signed in (bit n = user n). They stay signed in.
// A guest has the console's guest flag in its sign-in info (dwInfoFlags bit 1), the guest number and user 0 as its
// sponsor; it has no profile settings and no content of its own.
std::atomic<uint32_t> g_guests{0};

bool IsGuest(uint32_t user_index) {
  return user_index >= 1 && user_index < 4 && ((g_guests.load(std::memory_order_relaxed) >> user_index) & 1);
}

// The profile's XUID with the guest number in bits 54 and 55 (distinct from the profile's and from each other).
uint64_t GuestXuid(uint32_t user_index) {
  return REX_KERNEL_STATE()->user_profile()->xuid() ^ (uint64_t(user_index) << 54);
}

// "Name(1)", as the console names guests; at most 15 characters.
std::string GuestName(uint32_t user_index) {
  std::string name = REX_KERNEL_STATE()->user_profile()->name();
  if (name.size() > 12) name.resize(12);
  return name + "(" + std::to_string(user_index) + ")";
}

// culdcept-nx (2026-11-19, xam_user_trace): one line when the answer to (question, user) changes.
void TraceUser(const char* what, uint32_t user_index, uint64_t answer, uint64_t detail = 0) {
  if (!REXCVAR_GET(xam_user_trace) || user_index == 0 || user_index > 4) return;  // 4: user 0 (the caller trace)
  static std::mutex mutex;
  static std::map<std::pair<std::string, uint32_t>, std::pair<uint64_t, int64_t>> last;
  std::lock_guard<std::mutex> lock(mutex);
  const auto key = std::make_pair(std::string(what), user_index);
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto it = last.find(key);
  // Logged when the answer changes, and otherwise at most once a second while the game keeps asking.
  if (it != last.end() && it->second.first == (answer ^ (detail << 1)) && now - it->second.second < 1000) return;
  last[key] = {answer ^ (detail << 1), now};
  REXKRNL_INFO("[usertrace] {}(user {}) -> {:X} ({:X})", what, user_index, answer, detail);
}

// Signs in every connected controller beyond the first that is not signed in yet; the mask of all signed-in users.
uint32_t SignInGuests() {
  uint32_t guests = g_guests.load(std::memory_order_relaxed);
  if (REXCVAR_GET(xam_guest_signin)) {
    auto* input = static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
    for (uint32_t user = 1; user < 4 && input; ++user) {
      rex::input::X_INPUT_STATE state;
      if (((guests >> user) & 1) || input->GetState(user, &state) != X_ERROR_SUCCESS) continue;
      guests |= 1u << user;
      REXKRNL_INFO("[XAM] controller {} signed in as guest {} of the profile", user + 1, GuestName(user));
    }
    g_guests.store(guests, std::memory_order_relaxed);
  }
  return 1u | guests;
}

// culdcept-nx (2026-11-19, the user's second controller in Culdcept Saga's Versus lobby): a controller that connects
// after the game's sign-in screen is signed in as a guest too, and the game told (XN_SYS_SIGNINCHANGED), as a player
// signing in through the Guide would. The lobby opens the sign-in screen once; a pad switched on after it had no profile
// and its presses were ignored. Checked from XamInputGetState, at most every half second.
std::atomic<bool> g_signin_screen_shown{false};

}  // namespace

void SignInLateGuests() {
  if (!g_signin_screen_shown.load(std::memory_order_relaxed) || !REXCVAR_GET(xam_guest_signin)) return;
  static std::atomic<int64_t> next_ms{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t next = next_ms.load(std::memory_order_relaxed);
  if (now < next || !next_ms.compare_exchange_strong(next, now + 500)) return;
  const uint32_t before = g_guests.load(std::memory_order_relaxed);
  const uint32_t mask = SignInGuests();
  if ((mask & ~1u) != before) {
    REXKRNL_INFO("[XAM] signed in after the sign-in screen: users {:04b}; the game told (XN_SYS_SIGNINCHANGED)", mask);
    REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, mask);
  }
}

i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  assert_true(type_mask == 1 || type_mask == 2 || type_mask == 3 || type_mask == 4 ||
              type_mask == 7);
  if (!xuid_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      auto type = user_profile->type() & type_mask;
      if (type & (2 | 4)) {
        // maybe online profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      } else if (type & 1) {
        // maybe offline profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      }
    } else if (IsGuest(user_index)) {
      xuid = GuestXuid(user_index);
      result = X_E_SUCCESS;
    }
  } else {
    result = X_E_INVALIDARG;
  }
  *xuid_ptr = xuid;
  TraceUser("XamUserGetXUID", user_index, result, xuid);
  return result;
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  uint32_t signin_state = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      signin_state = user_profile->signin_state();
    } else if (IsGuest(user_index)) {
      signin_state = 1;  // signed in locally
    }
  }
  if (REXCVAR_GET(xam_user_trace) && user_index < 4) {  // culdcept-nx: the guest caller, once a second per (user, caller)
    auto* ppc = rex::runtime::current_ppc_context();
    const uint32_t lr = uint32_t(ppc->lr);
    // The caller's caller: a leaf-framed wrapper (stwu -96, lr saved at -8 of the caller's r1) keeps it at r1 + 88.
    const uint32_t r1 = uint32_t(ppc->r1.u32);
    const uint32_t up = r1 ? rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(r1 + 88)) : 0;
    char what[64];
    std::snprintf(what, sizeof(what), "XamUserGetSigninState from %08X < %08X", lr, up);
    TraceUser(what, user_index == 0 ? 4 : user_index, signin_state);
  }
  TraceUser("XamUserGetSigninState", user_index, signin_state);
  return signin_state;
}

typedef struct {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> unk08;  // maybe zero?
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;  // ?
  rex::be<uint32_t> unk14;  // ?
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (IsGuest(user_index)) {
    info->xuid = GuestXuid(user_index);
    info->unk08 = 2;  // dwInfoFlags: XUSER_INFO_FLAG_GUEST
    info->signin_state = 1;
    info->unk10 = static_cast<uint32_t>(user_index);  // dwGuestNumber
    info->unk14 = 0;                                  // dwSponsorUserIndex
    rex::string::copy_truncating(info->name, GuestName(user_index), rex::countof(info->name));
    TraceUser("XamUserGetSigninInfo", user_index, X_E_SUCCESS, flags);
    return X_E_SUCCESS;
  }
  if (user_index) {
    TraceUser("XamUserGetSigninInfo", user_index, X_E_NO_SUCH_USER, flags);
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  info->xuid = user_profile->xuid();
  info->signin_state = user_profile->signin_state();
  rex::string::copy_truncating(info->name, user_profile->name(), rex::countof(info->name));
  return X_E_SUCCESS;
}

u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (IsGuest(user_index)) {
    rex::string::copy_truncating(buffer, GuestName(user_index), std::min(buffer_len, uint32_t(16)));
    return X_E_SUCCESS;
  }
  if (user_index) {
    // peggle1-nx (2026-10-21): Peggle's same-console duel lets controller 2 join without a sign-in and shows what
    // XUserGetName left in its stack buffer without looking at the result (sub_82645660): "S?ÿB" as player 2's name.
    // With xam_guest_signin, a connected controller without a profile is named as its guest would be.
    auto* input = static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
    rex::input::X_INPUT_STATE state;
    if (REXCVAR_GET(xam_guest_signin) && input && input->GetState(user_index, &state) == X_ERROR_SUCCESS) {
      rex::string::copy_truncating(buffer, GuestName(user_index), std::min(buffer_len, uint32_t(16)));
      return X_E_SUCCESS;
    }
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const auto& user_name = user_profile->name();
  rex::string::copy_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index && !IsGuest(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  auto user_name = rex::string::to_utf16(user_index ? GuestName(user_index) : user_profile->name());
  rex::string::copy_and_swap_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

typedef struct {
  rex::be<uint32_t> setting_count;
  rex::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                                      be<uint64_t>* xuids, uint32_t setting_count,
                                      be<uint32_t>* setting_ids, uint32_t unk,
                                      be<uint32_t>* buffer_size_ptr, uint8_t* buffer,
                                      XAM_OVERLAPPED* overlapped) {
  if (!xuid_count) {
    assert_null(xuids);
  } else {
    assert_true(xuid_count == 1);
    assert_not_null(xuids);
    // TODO(gibbed): allow proper lookup of arbitrary XUIDs
    const auto& user_profile = REX_KERNEL_STATE()->user_profile();
    assert_true(static_cast<uint64_t>(xuids[0]) == user_profile->xuid());
    // TODO(gibbed): we assert here, but in case a title passes xuid_count > 1
    // until it's implemented for release builds...
    xuid_count = 1;
  }
  assert_zero(unk);  // probably flags

  // must have at least 1 to 32 settings
  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // buffer size pointer must be valid
  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // if buffer size is non-zero, buffer pointer must be valid
  auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  // Title ID = 0 means us.
  // 0xfffe07d1 = profile?

  // peggle2-nx (2026-10-20): a guest reads the profile's settings, as a guest on the console gets settings. Without
  // them Peggle 2 made no game profile for a guest (its sign-in handler reads every signed-in user's settings), and in a
  // local duel player 2's turns then took player 1's controller (player 2's own did nothing; the lobby, which reads
  // the controllers directly, worked).
  if (!xuids && user_index && !IsGuest(user_index)) {
    // Only support user 0 (and its guests).
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // TODO(gibbed): setting validity checking without needing a user profile
  // object.
  bool any_missing = false;
  for (uint32_t i = 0; i < setting_count; ++i) {
    auto setting_id = static_cast<uint32_t>(setting_ids[i]);
    auto setting = user_profile->GetSetting(setting_id);
    if (!setting) {
      any_missing = true;
      REXKRNL_ERROR(
          "xeXamUserReadProfileSettingsEx requested unimplemented setting "
          "{:08X}",
          setting_id);
    }
  }
  if (any_missing) {
    // TODO(benvanik): don't fail? most games don't even check!
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_INVALID_PARAMETER;
  }

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr = REX_KERNEL_MEMORY()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(REX_KERNEL_MEMORY()->HostToGuestVirtual(buffer), buffer,
                                            buffer_size, needed_header_size);
  for (uint32_t n = 0; n < setting_count; ++n) {
    uint32_t setting_id = setting_ids[n];
    auto setting = user_profile->GetSetting(setting_id);

    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    out_setting->from = !setting || !setting->is_set ? 0 : setting->is_title_specific() ? 2 : 1;
    if (xuids) {
      out_setting->xuid = user_profile->xuid();
    } else {
      out_setting->user_index = static_cast<uint32_t>(user_index);
    }
    out_setting->setting_id = setting_id;

    if (setting && setting->is_set) {
      setting->Append(&out_setting->data, &out_stream);
    }
    ++out_setting;
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(
        REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, mapped_u64 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                     ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, 0, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       mapped_u64 xuids, u32 setting_count, mapped_u32 setting_ids,
                                       mapped_u32 buffer_size_ptr, u32 unk_2,
                                       mapped_void buffer_ptr,
                                       ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, unk_2, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count,
                                      ppc_ptr_t<X_USER_PROFILE_SETTING> settings,
                                      ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(),
                                                      X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  // Update and save settings.
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type = static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::UNSET) {
      continue;
    }

    REXKRNL_DEBUG(
        "XamUserWriteProfileSettings: setting index [{}]:"
        " from={} setting_id={:08X} data.type={}",
        n, (uint32_t)setting.from, (uint32_t)setting.setting_id, setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::CONTENT:
      case UserProfile::Setting::Type::BINARY: {
        uint8_t* binary_ptr = REX_KERNEL_MEMORY()->TranslateVirtual(setting.data.binary.ptr);
        size_t binary_size = setting.data.binary.size;
        std::vector<uint8_t> bytes;
        if (setting.data.binary.ptr) {
          // Copy provided data
          bytes.resize(binary_size);
          std::memcpy(bytes.data(), binary_ptr, binary_size);
        } else {
          // Data pointer was NULL, so just fill with zeroes
          bytes.resize(binary_size, 0);
        }
        user_profile->AddSetting(
            std::make_unique<xam::UserProfile::BinarySetting>(setting.setting_id, bytes));
      } break;
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::DOUBLE:
      case UserProfile::Setting::Type::FLOAT:
      case UserProfile::Setting::Type::INT32:
      case UserProfile::Setting::Type::INT64:
      case UserProfile::Setting::Type::DATETIME:
        if (REXCVAR_GET(banjo_xam_profile_all_types)) {
          // Port 159 (from fnxf_xiii): the other types kept as well.
          switch (setting_type) {
            case UserProfile::Setting::Type::INT32:
              user_profile->AddSetting(std::make_unique<xam::UserProfile::Int32Setting>(
                  setting.setting_id, int32_t(setting.data.s32)));
              break;
            case UserProfile::Setting::Type::INT64:
              user_profile->AddSetting(std::make_unique<xam::UserProfile::Int64Setting>(
                  setting.setting_id, int64_t(setting.data.s64)));
              break;
            case UserProfile::Setting::Type::DATETIME:
              user_profile->AddSetting(std::make_unique<xam::UserProfile::DateTimeSetting>(
                  setting.setting_id, int64_t(setting.data.s64)));
              break;
            case UserProfile::Setting::Type::DOUBLE:
              user_profile->AddSetting(std::make_unique<xam::UserProfile::DoubleSetting>(
                  setting.setting_id, double(setting.data.f64)));
              break;
            case UserProfile::Setting::Type::FLOAT:
              user_profile->AddSetting(std::make_unique<xam::UserProfile::FloatSetting>(
                  setting.setting_id, float(setting.data.f32)));
              break;
            default: {  // WSTRING
              std::u16string str;
              if (setting.data.unicode.ptr) {
                auto* chars = reinterpret_cast<const rex::be<uint16_t>*>(
                    REX_KERNEL_MEMORY()->TranslateVirtual(setting.data.unicode.ptr));
                const uint32_t count = uint32_t(setting.data.unicode.size) / 2;
                for (uint32_t i = 0; i < count && uint16_t(chars[i]); ++i)
                  str.push_back(char16_t(uint16_t(chars[i])));
              }
              user_profile->AddSetting(
                  std::make_unique<xam::UserProfile::UnicodeSetting>(setting.setting_id, str));
            } break;
          }
          break;
        }
        [[fallthrough]];
      default: {
        REXKRNL_ERROR("XamUserWriteProfileSettings: Unimplemented data type {}", setting_type);
      } break;
    };
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  // checking all users?
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return X_ERROR_INVALID_PARAMETER;
    }

    if (user_index && !IsGuest(user_index)) {
      TraceUser("XamUserCheckPrivilege", user_index, X_ERROR_NO_SUCH_USER, mask);
      return X_ERROR_NO_SUCH_USER;
    }
  }

  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;
  TraceUser("XamUserCheckPrivilege", user_index == 0xFF ? 1 : user_index, X_ERROR_SUCCESS, mask);
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetFlags_entry(u32 user_index, mapped_u32 out_flags) {
  if (user_index && !IsGuest(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetRating_entry(u32 user_index, u32 unk1, mapped_u32 out_unk2,
                                             mapped_u32 out_unk3) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionCheckAccess_entry(u32 user_index, u32 unk1, u32 unk2, u32 unk3,
                                               u32 unk4, mapped_u32 out_unk5, u32 overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}

u32 XamUserIsOnlineEnabled_entry(u32 user_index) {
  return 1;
}

u32 XamUserGetMembershipTier_entry(u32 user_index) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }
  return 6 /* 6 appears to be Gold */;
}

u32 XamUserAreUsersFriends_entry(u32 user_index, u32 unk1, u32 unk2, mapped_u32 out_value,
                                 u32 overlapped_ptr) {
  uint32_t are_friends = 0;
  X_RESULT result;

  if (user_index >= 4) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      if (user_profile->signin_state() == 0) {
        result = X_ERROR_NOT_LOGGED_ON;
      } else {
        // No friends!
        are_friends = 0;
        result = X_ERROR_SUCCESS;
      }
    } else {
      // Only support user 0.
      result = X_ERROR_NO_SUCH_USER;  // if user is local -> X_ERROR_NOT_LOGGED_ON
    }
  }

  if (out_value) {
    assert_true(!overlapped_ptr);
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!out_value);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(
        overlapped_ptr, result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result), result == X_ERROR_SUCCESS ? are_friends : 0);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamShowSigninUI_entry(u32 unk, u32 unk_mask) {
  if (REXCVAR_GET(xam_user_trace)) {
    REXKRNL_INFO("[usertrace] XamShowSigninUI(panes {}, flags {:08X})", uint32_t(unk), uint32_t(unk_mask));
  }
  // Mask values vary. Probably matching user types? Local/remote?

  // To fix game modes that display a 4 profile signin UI (even if playing
  // alone):
  // XN_SYS_SIGNINCHANGED, with the mask of the signed-in users: the profile, and as its guests the further
  // controllers that are connected now (xam_guest_signin)
  g_signin_screen_shown.store(true, std::memory_order_relaxed);  // culdcept-nx: later pads signed in too
  if (REXCVAR_GET(xam_signin_ui_opens)) {
    REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 1);  // XN_SYS_UI (on): the screen opens
  }
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, SignInGuests());
  // Games seem to sit and loop until we trigger this notification:
  // XN_SYS_UI (off)
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);
  return X_ERROR_SUCCESS;
}

// TODO(gibbed): probably a FILETIME/LARGE_INTEGER, unknown currently
struct X_ACHIEVEMENT_UNLOCK_TIME {
  rex::be<uint32_t> unk_0;
  rex::be<uint32_t> unk_4;
};

struct X_ACHIEVEMENT_DETAILS {
  rex::be<uint32_t> id;
  rex::be<uint32_t> label_ptr;
  rex::be<uint32_t> description_ptr;
  rex::be<uint32_t> unachieved_ptr;
  rex::be<uint32_t> image_id;
  rex::be<uint32_t> gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  rex::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);

class XStaticAchievementEnumerator : public XEnumerator {
 public:
  struct AchievementDetails {
    uint32_t id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    uint32_t image_id;
    uint32_t gamerscore;
    struct {
      uint32_t unk_0;
      uint32_t unk_4;
    } unlock_time;
    uint32_t flags;
  };

  XStaticAchievementEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                               uint32_t flags)
      : XEnumerator(kernel_state, items_per_enumerate,
                    sizeof(X_ACHIEVEMENT_DETAILS) +
                        (!!(flags & 7) ? X_ACHIEVEMENT_DETAILS::kStringBufferSize : 0)),
        flags_(flags) {}

  void AppendItem(AchievementDetails item) { items_.push_back(std::move(item)); }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset = items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<uint32_t>(string_offset), &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0, o = current_item_; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr = !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr = !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time.unk_0 = item.unlock_time.unk_0;
      details[i].unlock_time.unk_4 = item.unlock_time.unk_4;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    uint32_t ptr;
    uint8_t* data;
    size_t remaining_bytes;
  };

  uint32_t AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      assert_always();
      return 0;
    }
    auto ptr = sb.ptr;
    rex::string::copy_and_swap_truncating(reinterpret_cast<char16_t*>(sb.data), string, count);
    sb.ptr += static_cast<uint32_t>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

 private:
  uint32_t flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

// peggle1-nx (2026-10-21): the leaderboard enumerators (XUserCreateStatsEnumeratorByRank / ByXuid / ByRating) fail, as
// for a profile not signed in to Xbox Live: there are no leaderboards without it. A stub "returned" its first argument
// (the title id, 0 for the running title: success) and left the handle and the buffer size unwritten, so Peggle
// allocated a buffer of whatever size its stack held and enumerated a handle that was never made.
u32 XamUserCreateStatsEnumerator_entry(u32 title_id, u32 type, u64 pivot, u32 num_rows, u32 num_specs, u32 specs_ptr,
                                       mapped_u32 buffer_size_ptr, mapped_u32 handle_ptr) {
  (void)title_id;
  (void)type;
  (void)pivot;
  (void)num_rows;
  (void)num_specs;
  (void)specs_ptr;
  if (buffer_size_ptr) {
    *buffer_size_ptr = 0;
  }
  if (handle_ptr) {
    *handle_ptr = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamUserCreateAchievementEnumerator_entry(u32 title_id, u32 user_index, u32 xuid, u32 flags,
                                             u32 offset, u32 count, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(entry_size) * count;
  }

  auto e = object_ref<XStaticAchievementEnumerator>(
      new XStaticAchievementEnumerator(REX_KERNEL_STATE(), count, flags));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  // ACHIEVED | ACHIEVED_ONLINE flags the game checks to consider an achievement earned.
  constexpr uint32_t kAchievedFlags = 0x00030000;

  auto fill_unlock = [](XStaticAchievementEnumerator::AchievementDetails& item, uint32_t id,
                        const rex::system::KernelState* ks) {
    uint64_t ft = ks->GetAchievementUnlockTime(id);
    if (ft) {
      item.flags |= kAchievedFlags;
      item.unlock_time.unk_0 = static_cast<uint32_t>(ft & 0xFFFF'FFFF);
      item.unlock_time.unk_4 = static_cast<uint32_t>(ft >> 32);
    }
  };

  const auto* ks = REX_KERNEL_STATE();

  // Prefer the runtime store (populated from TOML or XDBF at boot) so that
  // dev-edited labels/descriptions are visible to the game's own queries.
  const auto store = ks->loaded_achievements();
  if (!store.empty()) {
    for (const auto& info : store) {
      auto item = XStaticAchievementEnumerator::AchievementDetails{
          info.id,
          rex::string::to_utf16(info.label),
          rex::string::to_utf16(info.description),
          rex::string::to_utf16(info.unachieved_description),
          info.image_id,
          info.gamerscore,
          {0, 0},
          info.flags};
      fill_unlock(item, info.id, ks);
      e->AppendItem(item);
    }
  } else {
    const util::XdbfGameData db = ks->title_xdbf();
    if (db.is_valid()) {
      const XLanguage language =
          db.GetExistingLanguage(static_cast<XLanguage>(rex::system::EffectiveLanguage()));
      for (const util::XdbfAchievementTableEntry& entry : db.GetAchievements()) {
        auto item = XStaticAchievementEnumerator::AchievementDetails{
            entry.id,
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.label_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.description_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.unachieved_id)),
            entry.image_id,
            entry.gamerscore,
            {0, 0},
            entry.flags};
        fill_unlock(item, entry.id, ks);
        e->AppendItem(item);
      }
    }
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamParseGamerTileKey_entry(mapped_u32 key_ptr, mapped_u32 out1_ptr, mapped_u32 out2_ptr,
                               mapped_u32 out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}

u32 XamReadTileToTexture_entry(u32 unknown, u32 title_id, u64 tile_id, u32 user_index,
                               mapped_void buffer_ptr, u32 stride, u32 height, u32 overlapped_ptr) {
  // TODO(gibbed): unknown=0,2,3,9
  if (!tile_id) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t size = size_t(stride) * size_t(height);
  std::memset(buffer_ptr, 0xFF, size);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamWriteGamerTile_entry(u32 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 overlapped_ptr) {
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  *handle_ptr = 0xCAFEDEAD;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  assert_true(handle == 0xCAFEDEAD);
  // TODO(PermaNull): Implement this properly,
  // For the time being returning 0xDEADF00D will prevent crashing.
  *obj_ptr = 0xDEADF00D;
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserGetXUID, rex::kernel::xam::XamUserGetXUID_entry)
REX_EXPORT(__imp__XamUserGetSigninState, rex::kernel::xam::XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, rex::kernel::xam::XamUserGetSigninInfo_entry)
REX_EXPORT(__imp__XamUserGetName, rex::kernel::xam::XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, rex::kernel::xam::XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamUserReadProfileSettings, rex::kernel::xam::XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx,
           rex::kernel::xam::XamUserReadProfileSettingsEx_entry)
REX_EXPORT(__imp__XamUserWriteProfileSettings, rex::kernel::xam::XamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__XamUserCheckPrivilege, rex::kernel::xam::XamUserCheckPrivilege_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetFlags,
           rex::kernel::xam::XamUserContentRestrictionGetFlags_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetRating,
           rex::kernel::xam::XamUserContentRestrictionGetRating_entry)
REX_EXPORT(__imp__XamUserContentRestrictionCheckAccess,
           rex::kernel::xam::XamUserContentRestrictionCheckAccess_entry)
REX_EXPORT(__imp__XamUserIsOnlineEnabled, rex::kernel::xam::XamUserIsOnlineEnabled_entry)
REX_EXPORT(__imp__XamUserGetMembershipTier, rex::kernel::xam::XamUserGetMembershipTier_entry)
REX_EXPORT(__imp__XamUserAreUsersFriends, rex::kernel::xam::XamUserAreUsersFriends_entry)
REX_EXPORT(__imp__XamShowSigninUI, rex::kernel::xam::XamShowSigninUI_entry)
REX_EXPORT(__imp__XamUserCreateAchievementEnumerator,
           rex::kernel::xam::XamUserCreateAchievementEnumerator_entry)
REX_EXPORT(__imp__XamParseGamerTileKey, rex::kernel::xam::XamParseGamerTileKey_entry)
REX_EXPORT(__imp__XamReadTileToTexture, rex::kernel::xam::XamReadTileToTexture_entry)
REX_EXPORT(__imp__XamWriteGamerTile, rex::kernel::xam::XamWriteGamerTile_entry)
REX_EXPORT(__imp__XamSessionCreateHandle, rex::kernel::xam::XamSessionCreateHandle_entry)
REX_EXPORT(__imp__XamSessionRefObjByHandle, rex::kernel::xam::XamSessionRefObjByHandle_entry)

REX_EXPORT_STUB(__imp__XamUserAddRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserAllowedToPostToSocialNetwork);
REX_EXPORT_STUB(__imp__XamUserCreateAvatarAssetEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreatePlayerEnumerator);
REX_EXPORT(__imp__XamUserCreateStatsEnumerator, rex::kernel::xam::XamUserCreateStatsEnumerator_entry)
REX_EXPORT_STUB(__imp__XamUserCreateTitlesPlayedEnumerator);
REX_EXPORT_STUB(__imp__XamUserFlushLogonQueue);
REX_EXPORT_STUB(__imp__XamUserGetAge);
REX_EXPORT_STUB(__imp__XamUserGetAgeGroup);
REX_EXPORT_STUB(__imp__XamUserGetCachedUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetDeviceId);
REX_EXPORT_STUB(__imp__XamUserGetIndexFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetMembershipTierFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineCountryFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineLanguageFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineXUIDFromOfflineXUID);
REX_EXPORT_STUB(__imp__XamUserGetReportingInfo);
REX_EXPORT_STUB(__imp__XamUserGetRequestedUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserGetUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetUserFlagsFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetUserTenure);
REX_EXPORT_STUB(__imp__XamUserGetUsersMissingAvatars);
REX_EXPORT_STUB(__imp__XamUserGetXUIDForTFA);
REX_EXPORT_STUB(__imp__XamUserInvalidateProfileSetting);
REX_EXPORT_STUB(__imp__XamUserIsGuest);
REX_EXPORT_STUB(__imp__XamUserIsLogonPreviewModeEnabled);
REX_EXPORT_STUB(__imp__XamUserIsParentalControlled);
REX_EXPORT_STUB(__imp__XamUserIsPartial);
REX_EXPORT_STUB(__imp__XamUserIsPartialProfile);
REX_EXPORT_STUB(__imp__XamUserIsUnsafeProgrammingAllowed);
REX_EXPORT_STUB(__imp__XamUserLockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserLogon);
REX_EXPORT_STUB(__imp__XamUserLogonEx);
REX_EXPORT_STUB(__imp__XamUserLookupDevice);
REX_EXPORT_STUB(__imp__XamUserNuiBind);
REX_EXPORT_STUB(__imp__XamUserNuiEnableBiometric);
REX_EXPORT_STUB(__imp__XamUserNuiGetEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForBind);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForSignin);
REX_EXPORT_STUB(__imp__XamUserNuiIsBiometricEnabled);
REX_EXPORT_STUB(__imp__XamUserNuiUnbind);
REX_EXPORT_STUB(__imp__XamUserOverrideBindingCallbacks);
REX_EXPORT_STUB(__imp__XamUserOverrideDeviceBindings);
REX_EXPORT_STUB(__imp__XamUserOverrideGlobalState);
REX_EXPORT_STUB(__imp__XamUserOverrideUserInfo);
REX_EXPORT_STUB(__imp__XamUserPrefetchProfileSettings);
REX_EXPORT_STUB(__imp__XamUserProfileSync);
REX_EXPORT_STUB(__imp__XamUserReadUserPreference);
REX_EXPORT_STUB(__imp__XamUserResetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserUnlockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserUpdateRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserValidateAvatarManifest);
REX_EXPORT_STUB(__imp__XamUserWriteUserPreference);
REX_EXPORT_STUB(__imp__XamVerifyPasscode);
