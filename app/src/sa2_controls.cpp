// Sonic Adventure 2: the control options from OpenSA2 (https://github.com/kittycache9/OpenSA2 by kittycache, GPL-3.0-only:
// runtime/tweaks/action-buttons.c and analog-fix.c, host/player/tweaks/deadzone.cpp, camera-invert.cpp and
// hold-toggle.cpp). Each has its key in sa2.toml: sa2_analog_fix and sa2_deadzone on by default, the others off. Off, a
// hook is one byte test and the original.
//
// OpenSA2 ports the 2012 PC game; this is the Xbox 360 game, so every address below was found again here and each
// function was read against OpenSA2's notes. They agree: the same controller records, the same action flags, the same
// action window and the same somersault. The differences are named where they matter.
//
// The game's input, as these options use it:
//   sub_8290BAB8      XamInputGetState for the game's pad layer (r3 the pad, r4 the XINPUT_STATE): the two callers
//                     (sub_82956900, sub_829569F0) are the only readers of the pads.
//   sub_82519058      the pad records: raw axis = 127.0 * the axis past the pad layer's deadzone, then * 128 / 72.
//   kControllers      the records per player (0x34 bytes): +8 the held buttons, +0x10 the press edges, +0x1C x1, +0x1E y1.
//                     B 0x002, Y 0x200, X 0x400 (table 0x82A29DD0 of sub_82519250).
//   sub_82157448      the action flags from those: kActionHeld and kActionPressed, a byte per player (B or X).
//   sub_82156FD0      WriteAnalogs: each player's stick angle and stroke.
//   sub_824B05C0      Sonic_CheckActionWindow, called 22 times by Sonic_ChecksForDamage (sub_824A4A30), which Sonic,
//                     Shadow, Amy and Metal Sonic run. r5 the CharObj2Base: +0 the player, +3 CharID2, +4 the actions
//                     offered, +12 how many, +13 the one picked, +14 the one taken.
//   sub_824B1180      Sonic_Somersault. r4 the CharObj2Base, r5 the SonicCharObj2: +872 (0x368) the hold.
//
// None of these is saved. They change how the game plays, so they are options, not enhancements (cpu/sa2_enhancements.h).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "cpu/banjo_hot_common.h"  // sa2-nx-ult: the base's guest-memory helpers (Raw, Card)

REXCVAR_DEFINE_BOOL(sa2_action_buttons, false, "SA2/Controls",
                    "The light dash moves to Y, so B or X at a line of light-dash rings somersaults or starts a spin "
                    "dash as anywhere else; holding X starts Sonic's or Shadow's spin dash at once, and B keeps the "
                    "usual hold (from OpenSA2; the X part is the SA2 Mod Loader code \"Press B to Somersault\" by Hyper "
                    "and MainMemory)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_analog_fix, true, "SA2/Controls",
                    "The stick reaches full speed at about two thirds of a push instead of before half, so half a push "
                    "walks and a full push still runs (from OpenSA2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_deadzone, true, "SA2/Controls",
                    "A round deadzone for each pad's left stick, and full tilt a little before the stick's edge, the "
                    "same in every direction; the game's own deadzone still applies after it (from OpenSA2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_camera_invert, false, "SA2/Controls",
                    "The camera turns the other way: the right stick's left and right swap, and so do LT and RT, also "
                    "in the menus that read them (from OpenSA2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_hold_toggle, false, "SA2/Controls",
                    "B as a toggle: one press holds it and the next lets it go, for a mech's lock-on or the spin dash's "
                    "charge; START also lets it go (from OpenSA2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// A scripted test run has no line of light-dash rings at hand.
REXCVAR_DEFINE_BOOL(sa2_diag_offer_light_dash, false, "SA2/Diagnostics",
                    "Tests (sa2_action_buttons): every empty action window of Sonic's offers the light dash")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

// sa2-nx-ult (2026-11-19): the base's helpers (cpu/banjo_hot_common.h) in place of sa2-nx's sa2::hot ones: Raw (a
// guest address in host memory, the physical windows' offset included) and Card (the store cards of a written range).
using banjo::hot::Card;
inline uint8_t* Raw(uint8_t* base, uint32_t a) { return banjo::hot::Raw(base, a); }
inline const uint8_t* Raw(const uint8_t* base, uint32_t a) { return banjo::hot::Raw(const_cast<uint8_t*>(base), a); }
inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}

constexpr uint32_t kControllers = 0x83BF3200;
constexpr uint32_t kPadStride = 0x34, kPadHeld = 0x08, kPadPress = 0x10, kPadX1 = 0x1C, kPadY1 = 0x1E;
constexpr uint32_t kButtonB = 0x002, kButtonY = 0x200, kButtonX = 0x400;
constexpr uint32_t kActionPressed = 0x83BF3100, kActionHeld = 0x83BF31F0;
constexpr uint32_t kCoPlayer = 0, kCoCharId2 = 3, kCoOffered = 4, kCoCount = 12, kCoPicked = 13, kCoTaken = 14;
constexpr int kLightDash = 59;
constexpr uint32_t kSonicHold = 872;
constexpr int16_t kHoldPast = 24;  // the somersault's compare: above it, the spin dash starts

// What a run does, in the log: the first of each kind, then nothing (a console log shows that an option really ran).
constexpr int kListed = 16;
std::atomic<int> g_said_dash{0}, g_said_pass{0}, g_said_spin{0};
bool Listed(std::atomic<int>& said) {
  const int n = said.load(std::memory_order_relaxed);
  if (n >= kListed) return false;
  said.store(n + 1, std::memory_order_relaxed);
  return true;
}

inline uint8_t Ld8(const uint8_t* base, uint32_t a) { return *Raw(base, a); }
inline uint16_t Ld16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Raw(base, a), 2);
  return __builtin_bswap16(v);
}
inline float LdF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Ld32(base, a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void St8(uint8_t* base, uint32_t a, uint8_t v) {
  *Raw(base, a) = v;
  Card(base, a, 1);
}
inline void St16(uint8_t* base, uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(Raw(base, a), &v, 2);
  Card(base, a, 2);
}
inline void StF(uint8_t* base, uint32_t a, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(Raw(base, a), &u, 4);
  Card(base, a, 4);
}

// ---- The pad as the game reads it: sa2_deadzone, sa2_camera_invert, sa2_hold_toggle ----------------------------------

// XINPUT_STATE in guest memory: +0 the packet number, then XINPUT_GAMEPAD.
constexpr uint32_t kStateButtons = 4, kStateLt = 6, kStateRt = 7, kStateLx = 8, kStateLy = 10, kStateRx = 12;
constexpr uint16_t kXinputStart = 0x0010, kXinputB = 0x2000;
constexpr uint32_t kPads = 4;

// sa2_deadzone (OpenSA2's numbers): the centre up to 12 % of the reach, full tilt from 95 %, scaled between, the
// direction kept.
constexpr float kStickFull = 32767.0f, kStickInner = 0.12f * kStickFull, kStickOuter = 0.95f * kStickFull;

void RoundDeadzone(uint8_t* base, uint32_t state) {
  const float x = float(int16_t(Ld16(base, state + kStateLx))), y = float(int16_t(Ld16(base, state + kStateLy)));
  const float r = std::sqrt(x * x + y * y);
  if (r <= kStickInner) {
    St16(base, state + kStateLx, 0);
    St16(base, state + kStateLy, 0);
    return;
  }
  const float reach = r >= kStickOuter ? kStickFull : (r - kStickInner) / (kStickOuter - kStickInner) * kStickFull;
  // |x| <= r, so neither axis passes 32767 in magnitude.
  St16(base, state + kStateLx, uint16_t(int16_t(std::lround(x / r * reach))));
  St16(base, state + kStateLy, uint16_t(int16_t(std::lround(y / r * reach))));
}

// sa2_camera_invert: the game's L and R, digital and analog, are each made of a trigger and the right stick's X toward
// its side (sub_82519550, as OpenSA2 read the PC game's 0x00425700), so negating X and swapping the triggers swaps L and
// R for every reader of them. The game's mode is not known here: menus read them swapped too.
void CameraInvert(uint8_t* base, uint32_t state) {
  const int16_t rx = int16_t(Ld16(base, state + kStateRx));
  St16(base, state + kStateRx, uint16_t(rx == INT16_MIN ? INT16_MAX : int16_t(-rx)));
  const uint8_t lt = Ld8(base, state + kStateLt), rt = Ld8(base, state + kStateRt);
  St8(base, state + kStateLt, rt);
  St8(base, state + kStateRt, lt);
}

// sa2_hold_toggle: per pad, B latched and the buttons of the pad's last read. A press of B latches or lets go; START on
// any pad lets go of all (the pause menu and every other menu START opens). OpenSA2 also lets go when the window loses
// the focus, which the Switch has not, and when the pad is removed: here when its read fails. A latch from a menu stays
// held until the next B or START, since the game's mode is not known here.
bool g_latched[kPads] = {};
uint16_t g_raw[kPads] = {};

void HoldToggle(uint8_t* base, uint32_t pad, uint32_t state) {
  const uint16_t buttons = Ld16(base, state + kStateButtons);
  const uint16_t pressed = uint16_t(buttons & ~g_raw[pad]);
  g_raw[pad] = buttons;
  if (pressed & kXinputB) g_latched[pad] = !g_latched[pad];
  if (pressed & kXinputStart) std::fill(std::begin(g_latched), std::end(g_latched), false);
  St16(base, state + kStateButtons, uint16_t((buttons & ~kXinputB) | (g_latched[pad] ? kXinputB : 0)));
}

}  // namespace

REX_EXTERN(__imp__sub_8290BAB8);
REX_HOOK_RAW(sub_8290BAB8) {
  static const bool deadzone = REXCVAR_GET(sa2_deadzone), invert = REXCVAR_GET(sa2_camera_invert),
                    toggle = REXCVAR_GET(sa2_hold_toggle);
  if (!(deadzone || invert || toggle)) {
    __imp__sub_8290BAB8(ctx, base);
    return;
  }
  const uint32_t pad = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_8290BAB8(ctx, base);
  if (pad >= kPads) return;
  if (ctx.r3.u32 != 0) {  // no pad there
    g_latched[pad] = false;
    g_raw[pad] = 0;
    return;
  }
  if (deadzone) RoundDeadzone(base, state);
  if (invert) CameraInvert(base, state);
  if (toggle) HoldToggle(base, pad, state);
}

// ---- WriteAnalogs: sa2_analog_fix --------------------------------------------------------------------------------

namespace {

// Per player, 8 bytes: +0 the angle, +4 the stroke. kAnalog is what the game plays by (zero for a player whose pad it
// does not read), kAnalogAll the copy that is always written. kAnalogThrust: the stroke crossed 0.5 upward this frame.
constexpr uint32_t kAnalog = 0x83BF31A0, kAnalogAll = 0x83BF30C0, kAnalogThrust = 0x83BF33A0;
// With r the stick's length past the game's square deadzone of 14: stroke = r^3 / 125000 (the float 8e-6 at 0x820EB034,
// a constant in the generated code: tools/ro_constants.py), full at r = 50. A full push is r = 210 straight and 175 on a
// diagonal (127 * 128 / 72 - 14, the same as on the PC), so the stroke is full before half a push. OpenSA2's divisor:
// 112^3, full at r = 112, about two thirds of a push straight and four fifths on a diagonal; half a push gives 0.17.
constexpr float kStrokeScale = 1.0f / 1404928.0f;
constexpr float kDeadzone = 14.0f;

inline float PastDeadzone(float v) { return v > kDeadzone ? v - kDeadzone : v < -kDeadzone ? v + kDeadzone : 0.0f; }

}  // namespace

// The generated code holds the constant, so the strokes are worked out again after the original: its deadzone, angle
// and clamp stay the game's. Players 0 and 1 only (the original zeroes the others).
REX_EXTERN(__imp__sub_82156FD0);
REX_HOOK_RAW(sub_82156FD0) {
  static const bool on = REXCVAR_GET(sa2_analog_fix);
  if (!on) {
    __imp__sub_82156FD0(ctx, base);
    return;
  }
  const float before[2] = {LdF(base, kAnalog + 4), LdF(base, kAnalog + 8 + 4)};
  __imp__sub_82156FD0(ctx, base);
  for (uint32_t i = 0; i < 2; ++i) {
    if (LdF(base, kAnalogAll + 8 * i + 4) <= 0.0f) continue;  // inside the deadzone
    const uint32_t record = kControllers + kPadStride * i;
    const float x = PastDeadzone(float(int16_t(Ld16(base, record + kPadX1))));
    const float y = PastDeadzone(float(int16_t(Ld16(base, record + kPadY1))));
    const float r2 = x * x + y * y;
    const float stroke = std::min(std::sqrt(r2) * r2 * kStrokeScale, 1.0f);
    StF(base, kAnalogAll + 8 * i + 4, stroke);
    if (LdF(base, kAnalog + 8 * i + 4) != 0.0f) {
      StF(base, kAnalog + 8 * i + 4, stroke);
      St8(base, kAnalogThrust + i, stroke >= 0.5f && before[i] < 0.5f ? 1 : 0);
    }
  }
}

// ---- Sonic's action window and somersault: sa2_action_buttons ---------------------------------------------------

// Y light-dashes, B and X do not. The light dash is inlined in the window's body here (loc_824B0800; on the PC it is a
// function of its own), so a Y press runs the body with the flags that take that arm, the player's own flags back after
// it. It needs what it needs without the option: the light-dash rings offering it, and the Light Shoes.
// B or X with the light dash the action to take: the body would dash; the action is stored as taken and the answer is
// 0, as the body gives for an action its table does not list, so the caller goes on to the somersault.
REX_EXTERN(__imp__sub_824B05C0);
REX_HOOK_RAW(sub_824B05C0) {
  static const bool on = REXCVAR_GET(sa2_action_buttons);
  if (!on) {
    __imp__sub_824B05C0(ctx, base);
    return;
  }
  const uint32_t co2 = ctx.r5.u32;
  const int player = int8_t(Ld8(base, co2 + kCoPlayer));
  int count = int8_t(Ld8(base, co2 + kCoCount));
  if (player < 0 || player > 3 || count < 0 || count > 8) {
    __imp__sub_824B05C0(ctx, base);
    return;
  }
  if (static const bool offer = REXCVAR_GET(sa2_diag_offer_light_dash); offer && count == 0) [[unlikely]] {
    St8(base, co2 + kCoOffered, kLightDash);
    St8(base, co2 + kCoCount, 1);
    count = 1;
  }
  const uint32_t press = Ld32(base, kControllers + kPadStride * uint32_t(player) + kPadPress);
  const uint32_t pressed_at = kActionPressed + uint32_t(player);
  const uint8_t pressed = Ld8(base, pressed_at);
  const uint8_t picked_byte = Ld8(base, co2 + kCoPicked);
  const int picked = int8_t(picked_byte);
  bool offered = false, picked_offered = false;
  for (int i = 0; i < count; ++i) {
    const int action = int8_t(Ld8(base, co2 + kCoOffered + uint32_t(i)));
    offered = offered || action == kLightDash;
    picked_offered = picked_offered || action == picked;
  }
  if ((press & kButtonY) && offered) {
    St8(base, pressed_at, 1);
    St8(base, co2 + kCoPicked, kLightDash);
    __imp__sub_824B05C0(ctx, base);
    St8(base, pressed_at, pressed);
    St8(base, co2 + kCoPicked, picked_byte);
    if (Listed(g_said_dash)) REXLOG_INFO("[controls] sa2_action_buttons: player {}'s Y light-dashes", player);
    return;
  }
  if (pressed && count > 0 && (picked_offered ? picked : int8_t(Ld8(base, co2 + kCoOffered))) == kLightDash) {
    St8(base, co2 + kCoTaken, kLightDash);
    ctx.r3.u64 = 0;
    if (Listed(g_said_pass))
      REXLOG_INFO("[controls] sa2_action_buttons: player {}'s B or X at a light-dash line goes on to the somersault",
                  player);
    return;
  }
  __imp__sub_824B05C0(ctx, base);
}

// X held without B, for Sonic and Shadow, with the press's hold running: the hold is put past the somersault's compare
// for this call, so its next add takes the spin dash at once (the Mod Loader's code NOPs that compare while X is held).
// B keeps the usual hold. The hold goes back when the somersault did not read it. On the PC this wraps the somersault's
// caller; here the somersault itself, which is the hold's only reader in that call. Amy and Metal Sonic (CharID2 above 1)
// are left alone: they have no spin dash to start, and the hold is Metal Sonic's black shield.
REX_EXTERN(__imp__sub_824B1180);
REX_HOOK_RAW(sub_824B1180) {
  static const bool on = REXCVAR_GET(sa2_action_buttons);
  if (!on) {
    __imp__sub_824B1180(ctx, base);
    return;
  }
  const uint32_t co2 = ctx.r4.u32, sonic = ctx.r5.u32;
  const int player = int8_t(Ld8(base, co2 + kCoPlayer));
  if (player < 0 || player > 3 || Ld8(base, co2 + kCoCharId2) > 1) {
    __imp__sub_824B1180(ctx, base);
    return;
  }
  const uint32_t held = Ld32(base, kControllers + kPadStride * uint32_t(player) + kPadHeld);
  const int16_t hold = int16_t(Ld16(base, sonic + kSonicHold));
  if (!(held & kButtonX) || (held & kButtonB) || hold < 1 || !Ld8(base, kActionHeld + uint32_t(player)) ||
      Ld8(base, kActionPressed + uint32_t(player))) {
    __imp__sub_824B1180(ctx, base);
    return;
  }
  const int16_t preset = std::max(hold, kHoldPast);
  if (preset != hold) St16(base, sonic + kSonicHold, uint16_t(preset));
  __imp__sub_824B1180(ctx, base);
  if (int16_t(Ld16(base, sonic + kSonicHold)) == preset) {
    if (preset != hold) St16(base, sonic + kSonicHold, uint16_t(hold));
  } else if (Listed(g_said_spin)) {
    REXLOG_INFO("[controls] sa2_action_buttons: player {}'s held X starts the spin dash at once (hold {})", player,
                hold);
  }
}
