// sa2-nx-ult (2026-11-24, the user: "make sure all the options in SA2's release toml of previous version show up in the
// game menu"): every key of sa2-nx 1.0.6's shipped sa2.toml (app/sa2_switch.toml) on the game menu's Settings tab,
// through the menu's game hook (SetGameMenuRows, banjo-nx's way: its banjo_menu.cpp). The base's rows already have the
// language, the Xbox layout and the sideways Joy-Cons, and the MSAA row sets SA2's sa2_renut_msaa (game_menu.cpp);
// added here, beside them: relaxed vsync and double buffering (after MSAA), and OpenSA2's five control options (after
// the Joy-Con rows). The per-player button maps ([input_p1], [input_p2]) are the menu's own P1 / P2 Controls tabs.
// Every row but double buffering applies at once (those keys are read at each use); double buffering is the swapchain's
// image count, so it is marked * and written for the next start. Each default is the key's own.
#include "game_menu.h"
#include "game_menu_text.h"

#include <algorithm>
#include <string>
#include <vector>

namespace sa2::menu {
namespace {

using Setting = blur::GameMenuDialog::Setting;
using Choice = blur::GameMenuDialog::Choice;

Setting OnOff(const char* label, const char* key, bool on_by_default, bool restart, const char* help) {
  Setting x;
  x.label = label;
  x.key = key;
  x.restart = restart;
  x.always_in_file = true;  // 1.0.6's file has every one of them
  x.choices = {Choice{blur::GameMenuText(blur::C_ON), "true", "true"},
               Choice{blur::GameMenuText(blur::C_OFF), "false", "false"}};
  x.default_choice = on_by_default ? 0 : 1;
  x.help = help;
  return x;
}

// After the row of `key` (the last of `keys` found), or at the end.
void InsertAfter(std::vector<Setting>& s, std::initializer_list<const char*> keys, std::vector<Setting> rows) {
  auto at = s.end();
  for (const char* k : keys) {
    const auto it = std::find_if(s.begin(), s.end(), [&](const Setting& x) { return x.key == k; });
    if (it != s.end()) at = it + 1;
  }
  s.insert(at, std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
}

void Rows(std::vector<Setting>& s, int /*language_row*/, const std::string& /*file*/) {
  InsertAfter(s, {"sa2_renut_msaa"},
              {OnOff("Relaxed vsync", "sa2_vsync_relaxed", true, false,
                     "A frame that misses 60 fps is shown as soon as it is ready (50-59 fps) instead of dropping to 30."),
               OnOff("Double buffering", "sa2_renut_double_buffer", false, true,
                     "Two images instead of three: a finished frame reaches the screen sooner (less input lag), but a "
                     "frame that misses a refresh waits for the next one, so busy scenes (2-player) can drop frames.")});
  InsertAfter(s, {"input_xbox_layout", "input_joycon_sideways", "input_joycon_sideways_stick"},
              {OnOff("Analog fix", "sa2_analog_fix", true, false,
                     "The stick reaches full speed at about two thirds of a push instead of before half, so half a push "
                     "walks. Off: the game's own curve."),
               OnOff("Round deadzone", "sa2_deadzone", true, false,
                     "A round deadzone for each left stick, so a worn stick that rests off center stays still, and full "
                     "tilt a little before the stick's edge, the same in every direction."),
               OnOff("Light dash on Y", "sa2_action_buttons", false, false,
                     "The light dash moves to Y, so B or X at a line of light-dash rings somersaults or starts a spin "
                     "dash; holding X starts Sonic's or Shadow's spin dash at once. You still need the Light Shoes."),
               OnOff("Invert camera", "sa2_camera_invert", false, false,
                     "The camera turns the other way: the right stick's left and right swap, and so do ZL and ZR, also "
                     "in menus that read them."),
               OnOff("B as a toggle", "sa2_hold_toggle", false, false,
                     "One press of B holds it and the next lets it go (a mech's lock-on, the spin dash's charge). START "
                     "also lets it go.")});
}

}  // namespace

// From the app's OnCreateDialogs, before the menu is first drawn.
void Install() { blur::SetGameMenuRows(&Rows); }

}  // namespace sa2::menu
