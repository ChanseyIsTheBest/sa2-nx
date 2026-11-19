/**
 * @file        kernel/xam/xam_ui_switch.cpp
 * @brief       The Switch's software keyboard applet for XamShowKeyboardUI
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * banjo-nx (2026-10-21): when the game opens the Xbox 360 keyboard (XShowKeyboardUI), the Switch's own keyboard
 * (the system's swkbd library applet, through libnx) is shown instead of the ImGui text box, which has no keys to
 * type with on the console. Kept apart from xam_ui.cpp so <switch.h> stays out of the XAM headers.
 */

#include <rex/platform.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/string.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

namespace rex::kernel::xam {

// What became of one keyboard request (xam_ui.cpp).
enum class SwitchKeyboardResult { kAccepted, kCancelled, kUnavailable };

#if REX_PLATFORM_SWITCH

namespace {

// The Xbox 360 keyboard's flags that change the Switch keyboard (XDK xbox.h VKBD_*). The rest (the language sets,
// VKBD_HIGHLIGHT_TEXT, VKBD_SELECT_OK) have no Switch counterpart: the system's keyboard follows its own language.
constexpr uint32_t kVkbdLatinPhone = 0x00000008;
constexpr uint32_t kVkbdLatinIpAddress = 0x00000010;
constexpr uint32_t kVkbdLatinNumeric = 0x00000020;
constexpr uint32_t kVkbdLatinPassword = 0x00000080;

// swkbd draws a one-line field with the header and sub text only up to 32 characters; longer, it draws a box and
// shows neither (libnx swkbd.h, SwkbdTextDrawType).
constexpr uint32_t kLineDrawMax = 32;

// The press that closed the applet (A on OK, or plus) is often still held when the game has the controllers again;
// the game would take it as a new press (and could open the keyboard again). Waits until no button is held, at most
// this long.
constexpr auto kReleaseWait = std::chrono::milliseconds(1000);

void WaitForButtonsReleased() {
  PadState pad;
  padInitializeAny(&pad);
  const auto deadline = std::chrono::steady_clock::now() + kReleaseWait;
  while (std::chrono::steady_clock::now() < deadline) {
    padUpdate(&pad);
    if (padGetButtons(&pad) == 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

}  // namespace

// Shows the keyboard and waits for the player (the caller's thread blocks until the applet closes: the kernel's
// dispatch thread, a host thread that holds no lock, while the game keeps running as on the 360). max_chars is the
// most UTF-16 units the game's buffer takes without its terminator (1 or more). out_text is set when accepted.
SwitchKeyboardResult ShowSwitchKeyboard(const std::u16string& title, const std::u16string& description,
                                        const std::u16string& default_text, uint32_t flags, uint32_t max_chars,
                                        std::u16string& out_text) {
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_FAILED(rc)) {
    REXKRNL_WARN("Switch keyboard: swkbdCreate failed (0x{:08X})", rc);
    return SwitchKeyboardResult::kUnavailable;
  }
  // QWERTY (the system's language variant), cursor at the end, blurred game behind it.
  swkbdConfigMakePresetDefault(&kbd);
  if (flags & (kVkbdLatinNumeric | kVkbdLatinPhone | kVkbdLatinIpAddress)) {
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    if (flags & kVkbdLatinIpAddress) {
      swkbdConfigSetLeftOptionalSymbolKey(&kbd, ".");
    }
  }
  if (flags & kVkbdLatinPassword) {
    swkbdConfigSetPasswordFlag(&kbd, 1);
  }
  // One line, as the 360's keyboard (the preset allows newlines).
  swkbdConfigSetReturnButtonFlag(&kbd, 0);
  swkbdConfigSetStringLenMax(&kbd, max_chars);

  const std::string title8 = rex::string::to_utf8(title);
  const std::string description8 = rex::string::to_utf8(description);
  const std::string default8 = rex::string::to_utf8(default_text);
  if (max_chars <= kLineDrawMax) {
    swkbdConfigSetTextDrawType(&kbd, SwkbdTextDrawType_Line);
    swkbdConfigSetHeaderText(&kbd, title8.c_str());
    swkbdConfigSetSubText(&kbd, description8.c_str());
  } else {
    swkbdConfigSetTextDrawType(&kbd, SwkbdTextDrawType_Box);
  }
  // Shown in the empty field (a box has no header): the title, or the description when there is none.
  swkbdConfigSetGuideText(&kbd, (title8.empty() ? description8 : title8).c_str());
  swkbdConfigSetInitialText(&kbd, default8.c_str());

  // UTF-8 takes at most 3 bytes per UTF-16 unit (4 per surrogate pair), and swkbd stops at max_chars units.
  std::vector<char> text(size_t(max_chars) * 4 + 1, '\0');
  REXKRNL_INFO("Switch keyboard: shown (flags {:08X}, at most {} characters)", flags, max_chars);
  rc = swkbdShow(&kbd, text.data(), text.size());
  swkbdClose(&kbd);
  WaitForButtonsReleased();
  if (R_FAILED(rc)) {
    // The player's cancel closes the applet with a non-zero result, which libnx returns
    // as LibAppletBadExit; so does an applet that ended abnormally. Anything else: it could not be shown.
    if (rc == MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit)) {
      REXKRNL_INFO("Switch keyboard: cancelled");
      return SwitchKeyboardResult::kCancelled;
    }
    REXKRNL_WARN("Switch keyboard: swkbdShow failed (0x{:08X})", rc);
    return SwitchKeyboardResult::kUnavailable;
  }
  text.back() = '\0';
  out_text = rex::string::to_utf16(text.data());
  REXKRNL_INFO("Switch keyboard: accepted ({} characters)", out_text.size());
  return SwitchKeyboardResult::kAccepted;
}

#else

SwitchKeyboardResult ShowSwitchKeyboard(const std::u16string&, const std::u16string&, const std::u16string&,
                                        uint32_t, uint32_t, std::u16string&) {
  return SwitchKeyboardResult::kUnavailable;
}

#endif  // REX_PLATFORM_SWITCH

}  // namespace rex::kernel::xam
