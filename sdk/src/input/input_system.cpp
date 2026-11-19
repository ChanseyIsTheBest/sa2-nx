/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/frame_latency.h>  // banjo-nx (2026-10-19)
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>
#else
#include <rex/input/sdl/sdl_input_driver.h>
#endif
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

#if REX_PLATFORM_SWITCH
REXCVAR_DEFINE_STRING(input_backend, "switch", "Input", "Input backend: switch")
    .allowed({"switch"});
#else
REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});
#endif

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
REXCVAR_DEFINE_STRING(input_script, "", "Input",
                      "Test automation: button presses for user 0 as 'seconds:button,...' counted from the first input "
                      "poll (buttons A B X Y START BACK UP DOWN LEFT RIGHT LB RB), each held 0.2 s, or as long as "
                      "'@seconds' after the name says; the sticks pushed all the way with LSUP LSDOWN LSLEFT LSRIGHT "
                      "and RSUP RSDOWN RSLEFT RSRIGHT (e.g. '30:LSUP@2.5'), the triggers pulled all the way with LT RT "
                      "(e.g. '90:RT@300'). While set, user 0 always has a pad "
                      "connected (works minimized, with no controller)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(input_script_sticks, 0, "Input",
                     "Test automation with input_script (banjo-nx 2026-10-23): from this many seconds after the first "
                     "input poll, both analog sticks of user 0 wander at random (a new direction every 1.5 s, eased "
                     "between them): the left stick moves the player, the right turns the camera. 0: off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// culdcept-nx (2026-11-19, a second local player for a test route: Culdcept Saga's Versus join): user 1's presses.
REXCVAR_DEFINE_STRING(input_script_p2, "", "Input",
                      "Test automation: as input_script, for user 1 (the second controller): while set, user 1 always "
                      "has a pad connected")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(input_script_p3, "", "Input",
                      "Test automation: as input_script, for user 2 (the third controller); its pad connects 2 s "
                      "before its first press")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(input_script_p4, "", "Input",
                      "Test automation: as input_script, for user 3 (the fourth controller); its pad connects 2 s "
                      "before its first press")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// renut-ultimate (2026-11-10, the user's request): on by default. The Switch driver sends the game's two motors as HD
// rumble (the heavy one on the low band at 160 Hz, the light one on the high band at 320 Hz) to the handheld Joy-Cons,
// a Joy-Con pair, a single Joy-Con or a Pro Controller, and only when the value changes (one hid IPC then); the PC's
// SDL and XInput drivers pass it to the pad. Stopped when the process exits.
REXCVAR_DEFINE_BOOL(input_rumble, true, "Input",
                    "Controller vibration: the game's own rumble (on by default). false = controllers only get zero "
                    "vibration");
namespace rex::input {

namespace {
// blur-nx (2026-11-17, the Renut audit's #27, exact): the device list of a controller call kept per thread, so a call
// copies into a vector that already has room instead of making one (GetState, SetState and the others, several times
// a frame). DevicesForUser clears it first; none of these calls makes another while it holds the list.
std::vector<DeviceId>& UserDeviceScratch() {
  thread_local std::vector<DeviceId> ids;
  return ids;
}

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  /*
   * At most one enumeration every 50 ms.
   *
   * Stack sampling found the thread that prepares each game frame spending 5.8 % of its time
   * inside SetState (vibration), and every game call to the controller went through here:
   * three new vectors and a query to each driver, several times per frame. A controller is not
   * connected or disconnected 30 times per second; with 50 ms a new controller shows up before
   * anyone notices. The first call always enumerates.
   */
  {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const int64_t last = last_refresh_ns_.load(std::memory_order_relaxed);
    if (last != 0 && !devices_.empty() && now - last < 50000000) {
      return;
    }
    last_refresh_ns_.store(now, std::memory_order_relaxed);
  }
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
  // sa2-nx (banjo-nx 2026-10-21): drivers with per-player settings (the Switch's per-player button maps) learn each
  // device's player.
  if (changed) {
    for (auto& driver : drivers_) {
      driver->OnDevicesAssigned(devices_);
    }
  }
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

namespace {
// input_script: (seconds, buttons) presses, parsed once.
struct ScriptedPress {
  double at;
  uint16_t buttons;
  double hold = 0.2;
  // katamari-nx (2026-11-01): a stick pushed all the way (axis 0-3: left x, left y, right x, right y).
  int axis = -1;
  int16_t value = 0;
};
std::vector<ScriptedPress> ParseInputScript(const std::string& text);
const std::vector<ScriptedPress>& InputScript() {
  static const std::vector<ScriptedPress> presses = ParseInputScript(REXCVAR_GET(input_script));
  return presses;
}
// culdcept-nx (2026-11-19): input_script_p2, user 1's.
const std::vector<ScriptedPress>& InputScriptP2() {
  static const std::vector<ScriptedPress> presses = ParseInputScript(REXCVAR_GET(input_script_p2));
  return presses;
}
const std::vector<ScriptedPress>& InputScriptP3() {
  static const std::vector<ScriptedPress> presses = ParseInputScript(REXCVAR_GET(input_script_p3));
  return presses;
}
const std::vector<ScriptedPress>& InputScriptP4() {
  static const std::vector<ScriptedPress> presses = ParseInputScript(REXCVAR_GET(input_script_p4));
  return presses;
}
// User 1's, 2's or 3's script (input_script_p2 / _p3 / _p4); empty for any other user.
const std::vector<ScriptedPress>& InputScriptOther(uint32_t user_index) {
  static const std::vector<ScriptedPress> none;
  return user_index == 1 ? InputScriptP2() : user_index == 2 ? InputScriptP3() : user_index == 3 ? InputScriptP4() : none;
}
double ScriptSeconds();
// culdcept-nx (2026-11-19): a scripted user's pad connects 2 s before its first press (a pad switched on late).
bool ScriptOtherConnected(uint32_t user_index) {
  const auto& script = InputScriptOther(user_index);
  if (script.empty()) return false;
  double first = script.front().at;
  for (const auto& press : script) first = std::min(first, press.at);
  return ScriptSeconds() >= first - 2.0;
}
std::vector<ScriptedPress> ParseInputScript(const std::string& text) {
  {
    std::vector<ScriptedPress> out;
    size_t at = 0;
    while (at < text.size()) {
      // Port 176 (from rehatched-nx, its commit 19bf40fc): ';' and spaces separate presses too (the cvar layer keeps a
      // command-line string only up to its first comma, so a comma list scripted one press there).
      size_t end = text.find_first_of(",; ", at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      const size_t colon = item.find(':');
      if (colon == std::string::npos) continue;
      const double seconds = std::atof(item.substr(0, colon).c_str());
      std::string name = item.substr(colon + 1);
      double hold = 0.2;
      if (const size_t hold_at = name.find('@'); hold_at != std::string::npos) {
        hold = std::atof(name.substr(hold_at + 1).c_str());
        name.resize(hold_at);
      }
      static const std::pair<const char*, uint16_t> kButtons[] = {
          {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
          {"BACK", 0x0020},  {"LB", 0x0100},   {"RB", 0x0200},   {"A", 0x1000},     {"B", 0x2000},
          {"X", 0x4000},     {"Y", 0x8000}};
      for (const auto& [button, bits] : kButtons)
        if (name == button) out.push_back({seconds, bits, hold});
      static const struct {
        const char* name;
        int axis;
        int16_t value;
      } kSticks[] = {{"LSLEFT", 0, -32767}, {"LSRIGHT", 0, 32767}, {"LSDOWN", 1, -32767}, {"LSUP", 1, 32767},
                     {"RSLEFT", 2, -32767}, {"RSRIGHT", 2, 32767}, {"RSDOWN", 3, -32767}, {"RSUP", 3, 32767},
                     // blur-nx (2026-11-11): the triggers pulled all the way (Blur accelerates with RT: the PGO run
                     // drives through a race).
                     {"LT", 4, 255},        {"RT", 5, 255}};
      for (const auto& stick : kSticks)
        if (name == stick.name) out.push_back({seconds, 0, hold, stick.axis, stick.value});
    }
    if (!out.empty()) REXLOG_INFO("input_script: {} presses scripted", out.size());
    return out;
  }
}
// Seconds since the first input poll (the script's clock).
double ScriptSeconds() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
// banjo-nx (2026-10-23, input_script_sticks): the sticks' random wander: segment k of 1.5 s eases from target k to
// target k + 1, each target four values in [-28000, 28000] from a hash of k.
void ScriptedSticks(X_INPUT_GAMEPAD& pad) {
  static const int32_t from = REXCVAR_GET(input_script_sticks);
  if (from <= 0) return;
  const double t = ScriptSeconds() - double(from);
  if (t < 0.0) return;
  static double logged = -1.0;
  if (logged < 0.0) {
    logged = t;
    REXLOG_INFO("input_script: random stick movement from {:.1f} s", ScriptSeconds());
  }
  const uint64_t k = uint64_t(t / 1.5);
  const double f = t / 1.5 - double(k);
  const double ease = f * f * (3.0 - 2.0 * f);
  auto target = [](uint64_t segment, uint32_t axis) {
    uint64_t h = (segment * 4 + axis + 1) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    return double(int64_t(h % 56001) - 28000);
  };
  int16_t axes[4];
  for (uint32_t axis = 0; axis < 4; ++axis)
    axes[axis] = int16_t(target(k, axis) + (target(k + 1, axis) - target(k, axis)) * ease);
  pad.thumb_lx = axes[0];
  pad.thumb_ly = axes[1];
  pad.thumb_rx = axes[2];
  pad.thumb_ry = axes[3];
}
// Buttons the script holds now for user 0 (the clock starts at the first poll); culdcept-nx: or user 1's script.
uint16_t ScriptedButtons(const std::vector<ScriptedPress>& script = InputScript()) {
  const double now = ScriptSeconds();
  uint16_t buttons = 0;
  for (const ScriptedPress& press : script) {
    if (press.axis < 0 && now >= press.at && now < press.at + press.hold) {
      buttons |= press.buttons;
      static double logged = -1.0;
      if (logged != press.at) {
        logged = press.at;
        REXLOG_INFO("input_script: pressing {:04X} at {:.1f} s", press.buttons, now);
      }
    }
  }
  return buttons;
}
// The scripted stick pushes held now (katamari-nx, 2026-11-01); true when one is.
bool ScriptedStickPushes(X_INPUT_GAMEPAD& pad, const std::vector<ScriptedPress>& script = InputScript()) {
  const double now = ScriptSeconds();
  bool any = false;
  for (const ScriptedPress& press : script) {
    if (press.axis < 0 || now < press.at || now >= press.at + press.hold) continue;
    switch (press.axis) {
      case 0: pad.thumb_lx = press.value; break;
      case 1: pad.thumb_ly = press.value; break;
      case 2: pad.thumb_rx = press.value; break;
      case 3: pad.thumb_ry = press.value; break;
      case 4: pad.left_trigger = uint8_t(press.value); break;  // blur-nx (2026-11-11): LT
      default: pad.right_trigger = uint8_t(press.value); break;  // blur-nx: RT
    }
    any = true;
  }
  return any;
}
}  // namespace

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  if (out_caps && ((user_index == 0 && !InputScript().empty()) ||
                   ScriptOtherConnected(user_index))) {  // input_script (_p2, _p3): a standard pad
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xF3FF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    return X_ERROR_SUCCESS;
  }
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  RefreshDevices();
  std::vector<DeviceId>& ids = UserDeviceScratch();  // blur-nx (2026-11-17, audit #27)
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  // banjo-nx (2026-10-19, input latency): the late input delay (first read after a Swap) and the latency probe.
  rex::latency::BeforeInputRead();
  rex::latency::NoteInputRead();
  if (user_index == 0 && !InputScript().empty()) {  // input_script: only the scripted presses
    const uint16_t buttons = ScriptedButtons();
    static uint32_t packet = 1;
    static uint16_t last = 0;
    if (buttons != last) {
      last = buttons;
      ++packet;
    }
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet;
      out_state->gamepad.buttons = buttons;
      ScriptedSticks(out_state->gamepad);  // banjo-nx (2026-10-23)
      static bool was_pushed = false;
      const bool pushed = ScriptedStickPushes(out_state->gamepad);
      if (REXCVAR_GET(input_script_sticks) > 0 || pushed || was_pushed)
        out_state->packet_number = ++packet;  // the sticks change at every read (and once more on release)
      was_pushed = pushed;
    }
    return X_ERROR_SUCCESS;
  }
  if (user_index >= 1 && user_index <= 3 && !InputScriptOther(user_index).empty()) {  // culdcept-nx: _p2-_p4
    if (!ScriptOtherConnected(user_index)) return X_ERROR_DEVICE_NOT_CONNECTED;
    const uint16_t buttons = ScriptedButtons(InputScriptOther(user_index));
    static uint32_t packets[4] = {1, 1, 1, 1};
    static uint16_t lasts[4] = {};
    if (buttons != lasts[user_index]) {
      lasts[user_index] = buttons;
      ++packets[user_index];
    }
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packets[user_index];
      out_state->gamepad.buttons = buttons;
      static bool was_pushed[4] = {};
      const bool pushed = ScriptedStickPushes(out_state->gamepad, InputScriptOther(user_index));
      if (pushed || was_pushed[user_index]) out_state->packet_number = ++packets[user_index];
      was_pushed[user_index] = pushed;
    }
    return X_ERROR_SUCCESS;
  }
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  RefreshDevices();
  std::vector<DeviceId>& ids = UserDeviceScratch();  // blur-nx (2026-11-17, audit #27)
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  RefreshDevices();
  std::vector<DeviceId>& ids = UserDeviceScratch();  // blur-nx (2026-11-17, audit #27)
  assignment_->DevicesForUser(user_index, ids);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  // input_rumble = false: the game still sees its call serviced, but the controllers only
  // receive zero vibration.
  X_INPUT_VIBRATION no_rumble{};
  if (!REXCVAR_GET(input_rumble)) {
    vibration = &no_rumble;
  }
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_RESULT result = driver->SetDeviceVibration(id, vibration);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  if ((user_index == 0 || user_index == 0xFF) && !InputScript().empty()) {
    // input_script: menus read key events; one down / up event per change of the scripted buttons.
    static uint16_t reported = 0;
    const uint16_t buttons = ScriptedButtons();
    const uint16_t changed = uint16_t(buttons ^ reported);
    if (!changed) return X_ERROR_EMPTY;
    static const std::pair<uint16_t, uint16_t> kKeys[] = {
        {0x1000, 0x5800}, {0x2000, 0x5801}, {0x4000, 0x5802}, {0x8000, 0x5803}, {0x0200, 0x5804},
        {0x0100, 0x5805}, {0x0001, 0x5810}, {0x0002, 0x5811}, {0x0004, 0x5812}, {0x0008, 0x5813},
        {0x0010, 0x5814}, {0x0020, 0x5815}};
    for (const auto& [bit, virtual_key] : kKeys) {
      if (!(changed & bit)) continue;
      reported = uint16_t(reported ^ bit);
      if (out_keystroke) {
        std::memset(out_keystroke, 0, sizeof(*out_keystroke));
        out_keystroke->virtual_key = virtual_key;
        out_keystroke->flags = (buttons & bit) ? 0x0001 : 0x0002;  // XINPUT_KEYSTROKE_KEYDOWN / KEYUP
        out_keystroke->user_index = 0;
      }
      return X_ERROR_SUCCESS;
    }
    reported = buttons;
    return X_ERROR_EMPTY;
  }
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::lock_guard<std::recursive_mutex> devices_lock(devices_mutex_);  // sasrt-nx (2026-11-08): devices_mutex_
  RefreshDevices();
  std::vector<DeviceId>& ids = UserDeviceScratch();  // blur-nx (2026-11-17, audit #27)
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(input_backend) == "switch") {
      auto switch_driver = std::make_unique<nx::SwitchInputDriver>(nullptr, 0);
      if (switch_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(switch_driver));
      }
    }
#else
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }
#endif

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input
