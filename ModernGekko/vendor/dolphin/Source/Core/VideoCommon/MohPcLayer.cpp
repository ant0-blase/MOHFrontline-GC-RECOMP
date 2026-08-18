// Modern PC layer for Medal of Honor: Frontline (GMFE69).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/MohPcLayer.h"

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoConfig.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <imgui.h>

namespace MohPcLayer
{
namespace
{
constexpr double NATIVE_VPS = 59.94005994005994;
constexpr double NATIVE_ASPECT = 4.0 / 3.0;

// XKB keysyms used by the defaults. Printable Latin-1 keysyms equal ASCII.
constexpr u32 KEY_ESCAPE = 0xff1b;
constexpr u32 KEY_TAB = 0xff09;
constexpr u32 KEY_SPACE = 0x20;
constexpr u32 KEY_CTRL_L = 0xffe3;
constexpr u32 KEY_CTRL_R = 0xffe4;
constexpr u32 KEY_HOME = 0xff50;

enum class Action : size_t
{
  Forward,
  Back,
  Left,
  Right,
  Jump,
  Crouch,
  Use,
  Reload,
  Melee,
  PreviousWeapon,
  NextWeapon,
  CallHQ,
  CenterView,
  Pause,
  Count,
};

constexpr std::array<const char*, static_cast<size_t>(Action::Count)> ACTION_NAMES = {
    "Move forward", "Move backward", "Strafe left", "Strafe right", "Jump", "Crouch",
    "Action / Use", "Reload", "Melee", "Previous weapon", "Next weapon", "Call HQ",
    "Center view", "Pause"};

constexpr std::array<u32, static_cast<size_t>(Action::Count)> DEFAULT_KEYS = {
    'w', 's', 'a', 'd', KEY_SPACE, 'c', 'e', 'r', 'f', '1', '2', KEY_TAB, KEY_HOME, KEY_ESCAPE};

struct State
{
  std::atomic<bool> initialized{false};
  std::atomic<bool> gameplay{false};
  std::atomic<bool> settings_open{false};
  std::atomic<bool> input_enabled{true};
  std::atomic<bool> gamepad_enabled{true};
  std::atomic<bool> invert_y{false};
  std::atomic<bool> ui_safe{true};
  std::atomic<bool> adaptive_fps{true};
  std::atomic<bool> raw_mouse{true};
  std::atomic<float> sensitivity{1.0f};
  std::array<std::atomic<u32>, static_cast<size_t>(Action::Count)> keys{};
  std::array<std::atomic<bool>, 512> ascii_keys{};
  std::atomic<bool> ctrl_down{false};
  std::atomic<u32> mouse_buttons{0};
  std::atomic<double> rel_x{0.0};
  std::atomic<double> rel_y{0.0};
  std::atomic<int> wheel_up{0};
  std::atomic<int> wheel_down{0};
  std::atomic<int> menu_up{0};
  std::atomic<int> menu_down{0};
  std::atomic<int> menu_left{0};
  std::atomic<int> menu_right{0};
  std::atomic<double> last_abs_x{-1.0};
  std::atomic<double> last_abs_y{-1.0};
  std::atomic<int> window_width{1280};
  std::atomic<int> window_height{720};
  std::atomic<int> capture_action{-1};
  std::atomic<int> fire_button{0};
  std::atomic<int> aim_button{1};
  std::atomic<int> internal_resolution{3};
  std::atomic<int> audio_buffer_ms{120};
  std::atomic<int> audio_volume{100};
  std::atomic<bool> fill_audio_gaps{true};
  std::atomic<bool> preserve_audio_pitch{true};
  std::atomic<int> requested_fps{0}; // -1 original, 0 unlimited, >0 target.
  std::atomic<bool> fov_override{false};
  std::atomic<float> fov{90.0f};
  std::atomic<bool> weapon_follow{true};
  std::atomic<float> weapon_fov{100.0f};
  std::atomic<int> aspect_mode{0}; // 0 original,1 auto,2 16:10,3 16:9,4 21:9,5 32:9,6 custom
  std::atomic<int> aspect_num{16};
  std::atomic<int> aspect_den{9};
  std::string settings_path;
};

State s;

bool EnvTrue(const char* name, bool fallback)
{
  const char* v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  return std::string_view(v) != "0" && std::string_view(v) != "false" && std::string_view(v) != "off";
}

double EnvDouble(const char* name, double fallback)
{
  const char* v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  char* end = nullptr;
  const double parsed = std::strtod(v, &end);
  return end != v && *end == '\0' && std::isfinite(parsed) ? parsed : fallback;
}

void SetEnv(const char* name, const std::string& value)
{
  setenv(name, value.c_str(), 1);
}

void UnsetEnv(const char* name)
{
  unsetenv(name);
}

bool KeyDown(u32 sym)
{
  if (sym < s.ascii_keys.size())
    return s.ascii_keys[sym].load(std::memory_order_relaxed);
  if (sym == KEY_CTRL_L || sym == KEY_CTRL_R)
    return s.ctrl_down.load(std::memory_order_relaxed);
  return false;
}

bool ActionDown(Action a)
{
  const u32 sym = s.keys[static_cast<size_t>(a)].load(std::memory_order_relaxed);
  return KeyDown(sym);
}

void AddPulse(std::atomic<int>& pulse, int n = 2)
{
  pulse.store(std::max(pulse.load(std::memory_order_relaxed), n), std::memory_order_relaxed);
}

bool ConsumePulse(std::atomic<int>& pulse)
{
  int value = pulse.load(std::memory_order_relaxed);
  while (value > 0)
  {
    if (pulse.compare_exchange_weak(value, value - 1, std::memory_order_relaxed))
      return true;
  }
  return false;
}

std::string KeyName(u32 key)
{
  if (key >= 0x20 && key <= 0x7e)
  {
    char c = static_cast<char>(key);
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
    return std::string(1, c);
  }
  switch (key)
  {
  case KEY_ESCAPE: return "Escape";
  case KEY_TAB: return "Tab";
  case KEY_CTRL_L: return "Left Ctrl";
  case KEY_CTRL_R: return "Right Ctrl";
  case KEY_HOME: return "Home";
  default: return "Key 0x" + std::to_string(key);
  }
}

void ApplyAspect(int mode)
{
  int num = s.aspect_num.load();
  int den = s.aspect_den.load();
  if (mode == 0)
  {
    UnsetEnv("MOH_ASPECT_VALUE");
    UnsetEnv("MOH_ASPECT_NUM");
    UnsetEnv("MOH_ASPECT_DEN");
    UnsetEnv("MOH_ASPECT_AUTO");
    Config::SetCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Auto);
    return;
  }
  if (mode == 1)
  {
    num = std::max(s.window_width.load(), 1);
    den = std::max(s.window_height.load(), 1);
    SetEnv("MOH_ASPECT_AUTO", "1");
  }
  else
  {
    UnsetEnv("MOH_ASPECT_AUTO");
    switch (mode)
    {
    case 2: num = 16; den = 10; break;
    case 3: num = 16; den = 9; break;
    case 4: num = 21; den = 9; break;
    case 5: num = 32; den = 9; break;
    default: break;
    }
  }
  if (num <= 0 || den <= 0)
    return;
  s.aspect_num = num;
  s.aspect_den = den;
  SetEnv("MOH_CAMERA_PATCH", "1");
  SetEnv("MOH_ASPECT_NUM", std::to_string(num));
  SetEnv("MOH_ASPECT_DEN", std::to_string(den));
  SetEnv("MOH_ASPECT_VALUE", std::to_string(static_cast<double>(num) / den));
  SetEnv("MOH_UI_SAFE", s.ui_safe.load() ? "1" : "0");
  Config::SetCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Custom);
  Config::SetCurrent(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH, num);
  Config::SetCurrent(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT, den);
  Config::SetCurrent(Config::GFX_CROP_TO_ASPECT_RATIO, false);
  Config::SetCurrent(Config::GFX_WIDESCREEN_HACK, false);
}

void ApplyFov()
{
  if (s.fov_override.load())
  {
    SetEnv("MOH_CAMERA_PATCH", "1");
    SetEnv("MOH_FOV_DEGREES", std::to_string(s.fov.load()));
  }
  else
  {
    UnsetEnv("MOH_FOV_DEGREES");
  }
  if (s.weapon_follow.load())
    UnsetEnv("MOH_WEAPON_FOV_DEGREES");
  else
  {
    SetEnv("MOH_CAMERA_PATCH", "1");
    SetEnv("MOH_WEAPON_FOV_DEGREES", std::to_string(s.weapon_fov.load()));
  }
}

void ApplyFPS()
{
  const int fps = s.requested_fps.load();
  if (fps < 0)
  {
    UnsetEnv("MOH_TIMING_PATCH");
    UnsetEnv("MOH_FPS_TARGET");
    Config::SetCurrent(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
    return;
  }
  SetEnv("MOH_TIMING_PATCH", "1");
  if (fps == 0)
    SetEnv("MOH_FPS_TARGET", "unlimited");
  else
    SetEnv("MOH_FPS_TARGET", std::to_string(fps));
  const double requested = fps == 0 ? 1000.0 : std::max(fps, 1);
  Config::SetCurrent(Config::MAIN_VI_OVERCLOCK,
                     static_cast<float>(requested / NATIVE_VPS));
  Config::SetCurrent(Config::MAIN_PRECISION_FRAME_TIMING, true);
  Config::SetCurrent(Config::GFX_VSYNC, false);
}

void SaveSettings()
{
  if (s.settings_path.empty())
    return;
  std::ofstream f(s.settings_path, std::ios::trunc);
  if (!f)
    return;
  f << "sensitivity=" << s.sensitivity.load() << '\n';
  f << "invert_y=" << s.invert_y.load() << '\n';
  f << "gamepad=" << s.gamepad_enabled.load() << '\n';
  f << "adaptive_fps=" << s.adaptive_fps.load() << '\n';
  f << "ui_safe=" << s.ui_safe.load() << '\n';
  f << "fire_button=" << s.fire_button.load() << '\n';
  f << "aim_button=" << s.aim_button.load() << '\n';
  f << "internal_resolution=" << s.internal_resolution.load() << '\n';
  f << "audio_buffer=" << s.audio_buffer_ms.load() << '\n';
  f << "audio_volume=" << s.audio_volume.load() << '\n';
  f << "fill_audio_gaps=" << s.fill_audio_gaps.load() << '\n';
  f << "preserve_audio_pitch=" << s.preserve_audio_pitch.load() << '\n';
  f << "fps=" << s.requested_fps.load() << '\n';
  f << "fov_enabled=" << s.fov_override.load() << '\n';
  f << "fov=" << s.fov.load() << '\n';
  f << "weapon_follow=" << s.weapon_follow.load() << '\n';
  f << "weapon_fov=" << s.weapon_fov.load() << '\n';
  f << "aspect_mode=" << s.aspect_mode.load() << '\n';
  f << "aspect_num=" << s.aspect_num.load() << '\n';
  f << "aspect_den=" << s.aspect_den.load() << '\n';
  for (size_t i = 0; i < static_cast<size_t>(Action::Count); ++i)
    f << "key." << i << '=' << s.keys[i].load() << '\n';
}

void LoadSettings()
{
  for (size_t i = 0; i < static_cast<size_t>(Action::Count); ++i)
    s.keys[i] = DEFAULT_KEYS[i];
  if (const char* p = std::getenv("MOH_PC_SETTINGS_PATH"))
    s.settings_path = p;
  if (s.settings_path.empty())
    return;
  std::ifstream f(s.settings_path);
  if (!f)
    return;
  std::string line;
  while (std::getline(f, line))
  {
    const auto pos = line.find('=');
    if (pos == std::string::npos)
      continue;
    const std::string key = line.substr(0, pos);
    const std::string value = line.substr(pos + 1);
    auto as_i = [&] { return std::atoi(value.c_str()); };
    auto as_f = [&] { return static_cast<float>(std::atof(value.c_str())); };
    if (key == "sensitivity") s.sensitivity = std::clamp(as_f(), 0.05f, 10.0f);
    else if (key == "invert_y") s.invert_y = as_i() != 0;
    else if (key == "gamepad") s.gamepad_enabled = as_i() != 0;
    else if (key == "adaptive_fps") s.adaptive_fps = as_i() != 0;
    else if (key == "ui_safe") s.ui_safe = as_i() != 0;
    else if (key == "fire_button") s.fire_button = std::clamp(as_i(), 0, 2);
    else if (key == "aim_button") s.aim_button = std::clamp(as_i(), 0, 2);
    else if (key == "internal_resolution") s.internal_resolution = std::clamp(as_i(), 1, 8);
    else if (key == "audio_buffer") s.audio_buffer_ms = std::clamp(as_i(), 16, 512);
    else if (key == "audio_volume") s.audio_volume = std::clamp(as_i(), 0, 100);
    else if (key == "fill_audio_gaps") s.fill_audio_gaps = as_i() != 0;
    else if (key == "preserve_audio_pitch") s.preserve_audio_pitch = as_i() != 0;
    else if (key == "fps") s.requested_fps = as_i();
    else if (key == "fov_enabled") s.fov_override = as_i() != 0;
    else if (key == "fov") s.fov = std::clamp(as_f(), 20.0f, 150.0f);
    else if (key == "weapon_follow") s.weapon_follow = as_i() != 0;
    else if (key == "weapon_fov") s.weapon_fov = std::clamp(as_f(), 20.0f, 150.0f);
    else if (key == "aspect_mode") s.aspect_mode = std::clamp(as_i(), 0, 6);
    else if (key == "aspect_num") s.aspect_num = std::max(as_i(), 1);
    else if (key == "aspect_den") s.aspect_den = std::max(as_i(), 1);
    else if (key.rfind("key.", 0) == 0)
    {
      const int idx = std::atoi(key.c_str() + 4);
      if (idx >= 0 && idx < static_cast<int>(Action::Count))
        s.keys[idx] = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
    }
  }
}

std::optional<ControlState> InputOverride(std::string_view group, std::string_view control,
                                          ControlState original)
{
  if (!s.input_enabled.load())
    return std::nullopt;

  const bool block_game = s.settings_open.load();
  if (block_game)
    return static_cast<ControlState>(0.0);
  const bool keep_pad = s.gamepad_enabled.load();
  auto combine = [&](double pc) -> ControlState {
    return static_cast<ControlState>(std::clamp(keep_pad ? std::max<double>(original, pc) : pc, 0.0, 1.0));
  };

  if (group == GCPad::MAIN_STICK_GROUP)
  {
    if (block_game)
      return 0.0;
    // Keep both QWERTY and AZERTY comfortable out of the box.  Explicit
    // rebinding still remains authoritative; Z/Q are aliases for the default
    // W/A bindings only.
    const bool forward = ActionDown(Action::Forward) ||
                         (s.keys[static_cast<size_t>(Action::Forward)].load() == 'w' && KeyDown('z'));
    const bool left = ActionDown(Action::Left) ||
                      (s.keys[static_cast<size_t>(Action::Left)].load() == 'a' && KeyDown('q'));
    double x = (ActionDown(Action::Right) ? 1.0 : 0.0) - (left ? 1.0 : 0.0);
    double y = (forward ? 1.0 : 0.0) - (ActionDown(Action::Back) ? 1.0 : 0.0);
    if (!s.gameplay.load())
    {
      if (ConsumePulse(s.menu_up)) y = 1.0;
      if (ConsumePulse(s.menu_down)) y = -1.0;
      if (ConsumePulse(s.menu_left)) x = -1.0;
      if (ConsumePulse(s.menu_right)) x = 1.0;
    }
    if (control == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
    {
      if (x != 0.0 || !keep_pad)
        return static_cast<ControlState>(x);
      return std::nullopt;
    }
    if (control == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
    {
      if (y != 0.0 || !keep_pad)
        return static_cast<ControlState>(y);
      return std::nullopt;
    }
  }
  else if (group == GCPad::C_STICK_GROUP)
  {
    if (block_game)
      return 0.0;
    if (!s.gameplay.load())
    {
      if (keep_pad)
        return std::nullopt;
      return static_cast<ControlState>(0.0);
    }
    const double fps_factor = std::max<double>(1.0, Config::Get(Config::MAIN_VI_OVERCLOCK));
    const double gain = 0.018 * s.sensitivity.load() * fps_factor;
    if (control == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
    {
      const double dx = s.rel_x.exchange(0.0);
      if (dx != 0.0)
        return static_cast<ControlState>(std::clamp(dx * gain, -1.0, 1.0));
      if (keep_pad)
        return std::nullopt;
      return static_cast<ControlState>(0.0);
    }
    if (control == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
    {
      const double dy = s.rel_y.exchange(0.0);
      if (dy != 0.0)
      {
        double value = -dy * gain;
        if (s.invert_y.load()) value = -value;
        return static_cast<ControlState>(std::clamp(value, -1.0, 1.0));
      }
      if (keep_pad)
        return std::nullopt;
      return static_cast<ControlState>(0.0);
    }
  }
  else if (group == GCPad::BUTTONS_GROUP)
  {
    double pc = 0.0;
    const u32 buttons = s.mouse_buttons.load();
    if (control == GCPad::A_BUTTON)
      pc = ActionDown(Action::Use) || (!s.gameplay.load() && (buttons & 1u));
    else if (control == GCPad::B_BUTTON)
      pc = ActionDown(Action::Melee) || (!s.gameplay.load() && (buttons & 2u));
    else if (control == GCPad::X_BUTTON) pc = ActionDown(Action::Crouch) || KeyDown(KEY_CTRL_L) || KeyDown(KEY_CTRL_R);
    else if (control == GCPad::Y_BUTTON) pc = ActionDown(Action::Jump);
    else if (control == GCPad::Z_BUTTON) pc = ActionDown(Action::Reload);
    else if (control == GCPad::START_BUTTON) pc = ActionDown(Action::Pause);
    else return std::nullopt;
    return combine(pc);
  }
  else if (group == GCPad::TRIGGERS_GROUP)
  {
    const u32 buttons = s.mouse_buttons.load();
    const bool fire = (buttons & (1u << s.fire_button.load())) != 0;
    const bool aim = (buttons & (1u << s.aim_button.load())) != 0;
    if (control == GCPad::L_DIGITAL || control == GCPad::L_ANALOG)
      return combine(aim ? 1.0 : 0.0);
    if (control == GCPad::R_DIGITAL || control == GCPad::R_ANALOG)
      return combine(fire ? 1.0 : 0.0);
  }
  else if (group == GCPad::DPAD_GROUP)
  {
    double pc = 0.0;
    if (control == DIRECTION_UP)
      pc = ActionDown(Action::NextWeapon) || ConsumePulse(s.wheel_up);
    else if (control == DIRECTION_DOWN)
      pc = ActionDown(Action::PreviousWeapon) || ConsumePulse(s.wheel_down);
    else if (control == DIRECTION_LEFT)
      pc = ActionDown(Action::CenterView) || ((s.mouse_buttons.load() & 4u) != 0);
    else if (control == DIRECTION_RIGHT)
      pc = ActionDown(Action::CallHQ);
    else return std::nullopt;
    return combine(pc);
  }

  return std::nullopt;
}

void ResetDefaults()
{
  s.sensitivity = 1.0f;
  s.invert_y = false;
  s.gamepad_enabled = true;
  s.adaptive_fps = true;
  s.ui_safe = true;
  s.fire_button = 0;
  s.aim_button = 1;
  s.internal_resolution = 3;
  s.audio_buffer_ms = 120;
  s.audio_volume = 100;
  s.fill_audio_gaps = true;
  s.preserve_audio_pitch = true;
  for (size_t i = 0; i < static_cast<size_t>(Action::Count); ++i)
    s.keys[i] = DEFAULT_KEYS[i];
}

void SyncFromEnvironment()
{
  s.input_enabled = EnvTrue("MOH_PC_INPUT", true);
  s.sensitivity = static_cast<float>(std::clamp(EnvDouble("MOH_MOUSE_SENSITIVITY", s.sensitivity.load()), 0.05, 10.0));
  s.invert_y = EnvTrue("MOH_MOUSE_INVERT_Y", s.invert_y.load());
  s.ui_safe = EnvTrue("MOH_UI_SAFE", s.ui_safe.load());

  if (const char* fps = std::getenv("MOH_FPS_TARGET"))
  {
    if (std::string_view(fps) == "unlimited") s.requested_fps = 0;
    else s.requested_fps = std::clamp(std::atoi(fps), 1, 1000);
  }

  if (const char* fov = std::getenv("MOH_FOV_DEGREES"))
  {
    s.fov_override = true;
    s.fov = static_cast<float>(std::clamp(std::atof(fov), 20.0, 150.0));
  }
  if (const char* wfov = std::getenv("MOH_WEAPON_FOV_DEGREES"))
  {
    s.weapon_follow = false;
    s.weapon_fov = static_cast<float>(std::clamp(std::atof(wfov), 20.0, 150.0));
  }
  if (const char* n = std::getenv("MOH_ASPECT_NUM"))
  {
    const int num = std::max(std::atoi(n), 1);
    const int den = std::max(std::atoi(std::getenv("MOH_ASPECT_DEN") ? std::getenv("MOH_ASPECT_DEN") : "1"), 1);
    s.aspect_num = num;
    s.aspect_den = den;
    if (num == 16 && den == 10) s.aspect_mode = 2;
    else if (num == 16 && den == 9) s.aspect_mode = 3;
    else if (num == 21 && den == 9) s.aspect_mode = 4;
    else if (num == 32 && den == 9) s.aspect_mode = 5;
    else s.aspect_mode = 6;
  }
  if (EnvTrue("MOH_ASPECT_AUTO", false)) s.aspect_mode = 1;
}

} // namespace

void Initialize()
{
  if (s.initialized.exchange(true))
    return;
  s.requested_fps = -1;
  LoadSettings();
  SyncFromEnvironment();
  auto* config = Pad::GetConfig();
  if (config && config->GetController(0))
  {
    auto* pad = static_cast<GCPad*>(config->GetController(0));
    pad->SetInputOverrideFunction(InputOverride);
    std::fprintf(stderr, "[moh-pc] keyboard/mouse input layer installed (gamepad coexist=%s)\n",
                 s.gamepad_enabled.load() ? "on" : "off");
  }
}

void Shutdown()
{
  if (!s.initialized.exchange(false))
    return;
  SaveSettings();
}

void ApplyDolphinSettings()
{
  Config::SetBase(Config::GFX_EFB_SCALE, s.internal_resolution.load());
  Config::SetBase(Config::MAIN_AUDIO_BUFFER_SIZE, s.audio_buffer_ms.load());
  Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS, s.fill_audio_gaps.load());
  Config::SetBase(Config::MAIN_AUDIO_PRESERVE_PITCH, s.preserve_audio_pitch.load());
  Config::SetBase(Config::MAIN_AUDIO_VOLUME, s.audio_volume.load());
  ApplyFov();
  if (s.aspect_mode.load() != 0)
    ApplyAspect(s.aspect_mode.load());
  if (s.requested_fps.load() >= 0)
    ApplyFPS();
}

void SetGameplayActive(bool active)
{
  s.gameplay = active;
  s.rel_x = 0.0;
  s.rel_y = 0.0;
  s.last_abs_x = -1.0;
  s.last_abs_y = -1.0;
}

bool IsGameplayActive() { return s.gameplay.load(); }
bool IsSettingsOpen() { return s.settings_open.load(); }
bool WantsRelativeMouse()
{
  return s.input_enabled.load() && s.gameplay.load() && !s.settings_open.load();
}

void ToggleSettings()
{
  const bool open = !s.settings_open.load();
  s.settings_open = open;
  s.rel_x = 0.0;
  s.rel_y = 0.0;
  if (!open)
    SaveSettings();
  std::fprintf(stderr, "[moh-pc] settings menu %s\n", open ? "OPEN" : "CLOSED");
}

void SetWindowSize(int width, int height)
{
  s.window_width = std::max(width, 1);
  s.window_height = std::max(height, 1);
  if (s.aspect_mode.load() == 1)
    ApplyAspect(1);
}

void KeyEvent(u32 keysym, bool down)
{
  if (keysym < s.ascii_keys.size())
    s.ascii_keys[keysym] = down;
  if (keysym == KEY_CTRL_L || keysym == KEY_CTRL_R)
    s.ctrl_down = down;

  if (down)
  {
    const int capture = s.capture_action.exchange(-1);
    if (capture >= 0 && capture < static_cast<int>(Action::Count))
    {
      s.keys[capture] = keysym;
      SaveSettings();
    }
  }
}

void PointerAbsolute(double x, double y)
{
  if (!s.gameplay.load() && !s.settings_open.load())
  {
    // The original GameCube frontend has no cursor hit-test API.  Translate
    // actual mouse travel into discrete stick navigation and keep LMB/RMB as
    // A/B.  This makes every stock menu usable with a mouse without hardcoding
    // per-screen coordinates, while the PC settings overlay uses true ImGui
    // pointer hit-testing.
    const double last_x = s.last_abs_x.exchange(x);
    const double last_y = s.last_abs_y.exchange(y);
    if (last_x >= 0.0)
    {
      const double dx = x - last_x;
      if (dx > 20.0) AddPulse(s.menu_right);
      else if (dx < -20.0) AddPulse(s.menu_left);
    }
    if (last_y >= 0.0)
    {
      const double dy = y - last_y;
      if (dy > 16.0) AddPulse(s.menu_down);
      else if (dy < -16.0) AddPulse(s.menu_up);
    }
  }
}

void PointerButton(unsigned button, bool down)
{
  if (button > 2)
    return;
  u32 mask = s.mouse_buttons.load();
  const u32 bit = 1u << button;
  do
  {
    const u32 next = down ? (mask | bit) : (mask & ~bit);
    if (s.mouse_buttons.compare_exchange_weak(mask, next))
      break;
  } while (true);
}

void PointerAxis(double vertical_steps)
{
  if (vertical_steps < 0.0)
    AddPulse(s.wheel_up, 3);
  else if (vertical_steps > 0.0)
    AddPulse(s.wheel_down, 3);
}

void RelativeMotion(double dx, double dy)
{
  if (!WantsRelativeMouse())
    return;
  s.rel_x.store(s.rel_x.load(std::memory_order_relaxed) + dx, std::memory_order_relaxed);
  s.rel_y.store(s.rel_y.load(std::memory_order_relaxed) + dy, std::memory_order_relaxed);
}

void AdaptivePerformanceUpdate()
{
  if (!s.initialized.load() || !s.gameplay.load() || !s.adaptive_fps.load() ||
      !std::getenv("MOH_TIMING_PATCH") || !Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE))
    return;

  const int fps = s.requested_fps.load();
  const double target_fps = fps == 0 ? 1000.0 : (fps > 0 ? fps : NATIVE_VPS);
  const double target_factor = target_fps / NATIVE_VPS;
  const double current = Config::Get(Config::MAIN_VI_OVERCLOCK);
  const double max_speed = Core::System::GetInstance().GetPerfMetrics().GetMaxSpeed();
  if (!std::isfinite(max_speed) || max_speed <= 0.0)
    return;

  double next = current;
  if (max_speed < 0.985)
    next = std::max(1.0, current * max_speed * 0.96);
  else if (max_speed > 1.10 && current < target_factor)
    next = std::min(target_factor, current * std::min(1.08, max_speed * 0.97));
  if (std::fabs(next - current) > 0.015)
  {
    Config::SetCurrent(Config::MAIN_VI_OVERCLOCK, static_cast<float>(next));
    std::fprintf(stderr,
                 "[moh-pc] adaptive FPS/audio: VI factor %.3f -> %.3f (max-speed %.0f%%, ~%.1f FPS)\n",
                 current, next, max_speed * 100.0, next * NATIVE_VPS);
  }
}

void DrawSettingsUI(float backbuffer_scale)
{
  if (!s.settings_open.load())
    return;

  ImGui::SetNextWindowSize(ImVec2(680.0f * backbuffer_scale, 620.0f * backbuffer_scale),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(30.0f * backbuffer_scale, 30.0f * backbuffer_scale),
                          ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin("Medal of Honor: Frontline - PC Settings", &open))
  {
    ImGui::Text("Ctrl+F10 or ` toggles this menu. Changes are live.");
    ImGui::Separator();
    if (ImGui::BeginTabBar("moh_pc_tabs"))
    {
      if (ImGui::BeginTabItem("Graphics"))
      {
        int fps = s.requested_fps.load();
        const char* fps_items[] = {"Original", "60", "90", "120", "144", "165", "240", "Unlimited"};
        const int fps_values[] = {-1, 60, 90, 120, 144, 165, 240, 0};
        int fps_idx = 0;
        for (int i = 0; i < 8; ++i) if (fps_values[i] == fps) fps_idx = i;
        if (ImGui::Combo("FPS target", &fps_idx, fps_items, 8))
        {
          s.requested_fps = fps_values[fps_idx];
          ApplyFPS();
        }
        bool adaptive = s.adaptive_fps.load();
        if (ImGui::Checkbox("Adaptive FPS to protect audio / full-speed emulation", &adaptive))
          s.adaptive_fps = adaptive;

        bool fov_on = s.fov_override.load();
        if (ImGui::Checkbox("Custom world FOV", &fov_on))
        {
          s.fov_override = fov_on;
          ApplyFov();
        }
        float fov = s.fov.load();
        if (fov_on && ImGui::SliderFloat("World FOV", &fov, 50.0f, 130.0f, "%.1f deg"))
        {
          s.fov = fov;
          ApplyFov();
        }
        bool follow = s.weapon_follow.load();
        if (ImGui::Checkbox("Weapon FOV follows world", &follow))
        {
          s.weapon_follow = follow;
          ApplyFov();
        }
        float wfov = s.weapon_fov.load();
        if (!follow && ImGui::SliderFloat("Weapon FOV", &wfov, 50.0f, 130.0f, "%.1f deg"))
        {
          s.weapon_fov = wfov;
          ApplyFov();
        }

        const char* aspect_items[] = {"Original 4:3", "Auto (window)", "16:10", "16:9", "21:9", "32:9", "Custom"};
        int aspect = s.aspect_mode.load();
        if (ImGui::Combo("Aspect ratio", &aspect, aspect_items, 7))
        {
          s.aspect_mode = aspect;
          ApplyAspect(aspect);
        }
        if (aspect == 6)
        {
          int n = s.aspect_num.load(), d = s.aspect_den.load();
          bool changed = ImGui::InputInt("Aspect width", &n);
          changed |= ImGui::InputInt("Aspect height", &d);
          if (changed && n > 0 && d > 0)
          {
            s.aspect_num = n; s.aspect_den = d; ApplyAspect(6);
          }
        }
        bool safe = s.ui_safe.load();
        if (ImGui::Checkbox("Aspect-correct HUD / menus (4:3 safe area)", &safe))
        {
          s.ui_safe = safe;
          SetEnv("MOH_UI_SAFE", safe ? "1" : "0");
        }
        int ir = s.internal_resolution.load();
        if (ImGui::SliderInt("Internal resolution", &ir, 1, 8, "%dx"))
        {
          s.internal_resolution = ir;
          Config::SetCurrent(Config::GFX_EFB_SCALE, ir);
        }
        bool vsync = Config::Get(Config::GFX_VSYNC);
        if (ImGui::Checkbox("VSync", &vsync)) Config::SetCurrent(Config::GFX_VSYNC, vsync);
        ImGui::Text("Actual: %.1f FPS | VI %.1f Hz | Max speed %.0f%%",
                    Core::System::GetInstance().GetPerfMetrics().GetFPS(),
                    Config::Get(Config::MAIN_VI_OVERCLOCK) * NATIVE_VPS,
                    Core::System::GetInstance().GetPerfMetrics().GetMaxSpeed() * 100.0);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Keyboard / Mouse"))
      {
        bool enabled = s.input_enabled.load();
        if (ImGui::Checkbox("PC keyboard + mouse", &enabled)) s.input_enabled = enabled;
        bool pad = s.gamepad_enabled.load();
        if (ImGui::Checkbox("Allow gamepad at the same time", &pad)) s.gamepad_enabled = pad;
        float sens = s.sensitivity.load();
        if (ImGui::SliderFloat("Mouse sensitivity", &sens, 0.10f, 5.0f, "%.2f")) s.sensitivity = sens;
        bool invert = s.invert_y.load();
        if (ImGui::Checkbox("Invert mouse Y", &invert)) s.invert_y = invert;
        ImGui::TextDisabled("Default: WASD, LMB fire, RMB aim, E use, R reload, F melee,");
        ImGui::TextDisabled("Space jump, C/Ctrl crouch, wheel/1/2 weapons, Esc pause.");
        ImGui::SeparatorText("Bindings");
        for (size_t i = 0; i < static_cast<size_t>(Action::Count); ++i)
        {
          ImGui::PushID(static_cast<int>(i));
          ImGui::TextUnformatted(ACTION_NAMES[i]);
          ImGui::SameLine(230.0f * backbuffer_scale);
          const bool capturing = s.capture_action.load() == static_cast<int>(i);
          const std::string label = capturing ? "Press a key..." : KeyName(s.keys[i].load());
          if (ImGui::Button(label.c_str(), ImVec2(150.0f * backbuffer_scale, 0)))
            s.capture_action = static_cast<int>(i);
          ImGui::PopID();
        }
        const char* mouse_items[] = {"Left", "Right", "Middle"};
        int fire = s.fire_button.load();
        int aim = s.aim_button.load();
        if (ImGui::Combo("Fire mouse button", &fire, mouse_items, 3)) s.fire_button = fire;
        if (ImGui::Combo("Aim mouse button", &aim, mouse_items, 3)) s.aim_button = aim;
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Gamepad"))
      {
        bool pad = s.gamepad_enabled.load();
        if (ImGui::Checkbox("Enable physical gamepad alongside keyboard/mouse", &pad))
          s.gamepad_enabled = pad;
        ImGui::TextWrapped("The original GameCube pad remains fully supported. Keyboard/mouse is merged on top only when enabled, so you can switch devices at any time.");
        ImGui::TextDisabled("GameCube layout remains authoritative for gamepad prompts and rumble.");
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Audio"))
      {
        int volume = Config::Get(Config::MAIN_AUDIO_VOLUME);
        if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%"))
        {
          s.audio_volume = volume;
          Config::SetCurrent(Config::MAIN_AUDIO_VOLUME, volume);
        }
        int buffer = Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE);
        if (ImGui::SliderInt("Audio buffer", &buffer, 32, 256, "%d ms"))
        {
          s.audio_buffer_ms = buffer;
          Config::SetCurrent(Config::MAIN_AUDIO_BUFFER_SIZE, buffer);
        }
        bool gaps = Config::Get(Config::MAIN_AUDIO_FILL_GAPS);
        if (ImGui::Checkbox("Fill audio gaps", &gaps))
        {
          s.fill_audio_gaps = gaps;
          Config::SetCurrent(Config::MAIN_AUDIO_FILL_GAPS, gaps);
        }
        bool preserve = Config::Get(Config::MAIN_AUDIO_PRESERVE_PITCH);
        if (ImGui::Checkbox("Preserve audio pitch", &preserve))
        {
          s.preserve_audio_pitch = preserve;
          Config::SetCurrent(Config::MAIN_AUDIO_PRESERVE_PITCH, preserve);
        }
        ImGui::TextWrapped("Audio-safe FPS is enabled by default: if the PC cannot sustain the requested VI rate, the port lowers the effective render/VI rate instead of letting emulation fall below full speed. The game simulation still uses real-time delta, while pitch preservation and gap filling absorb short stalls.");
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("System"))
      {
        ImGui::Text("Renderer: Vulkan / ModernGekko StaticRecomp");
        ImGui::Text("Settings file: %s", s.settings_path.empty() ? "(not configured)" : s.settings_path.c_str());
        if (ImGui::Button("Save settings")) SaveSettings();
        ImGui::SameLine();
        if (ImGui::Button("Reset PC controls")) ResetDefaults();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  if (!open)
  {
    s.settings_open = false;
    SaveSettings();
  }
}

} // namespace MohPcLayer
