// Modern PC layer for Medal of Honor: Frontline (GMFE69).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/MohPcLayer.h"

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

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
  std::atomic<bool> invert_x{false};
  std::atomic<bool> invert_y{false};
  std::atomic<bool> ui_safe{true};
  std::atomic<bool> adaptive_fps{true};
  std::atomic<int> adaptive_profile{2}; // 0 off, 1 conservative, 2 balanced, 3 aggressive
  std::atomic<bool> raw_mouse{true};
  std::atomic<float> sensitivity{1.0f};
  std::atomic<float> sensitivity_x{1.0f};
  std::atomic<float> sensitivity_y{1.0f};
  std::atomic<float> ads_sensitivity{0.80f};

  // Optional modern FPS aim-down-sights. Original MOHF aiming remains the
  // gameplay authority; this layer only modernizes camera/viewmodel presentation.
  std::atomic<bool> fps_ads_enabled{false};
  std::atomic<float> ads_world_fov{72.0f};
  std::atomic<float> ads_weapon_fov{68.0f};
  std::atomic<float> ads_transition_ms{180.0f};
  std::atomic<bool> ads_hide_crosshair{true};
  std::atomic<float> ads_center_strength{1.0f};
  std::atomic<float> ads_target_x{0.0f};
  std::atomic<float> ads_target_y{0.0f};
  std::atomic<float> ads_z_offset{0.0f};
  std::atomic<float> ads_blend{0.0f};
  std::atomic<bool> gamepad_aim{false};
  std::atomic<int> current_weapon_type{-1};
  std::atomic<float> mouse_smoothing{0.0f};
  std::atomic<float> mouse_acceleration{0.0f};
  std::atomic<bool> toggle_aim{false};
  std::atomic<bool> toggle_crouch{false};
  std::atomic<bool> aim_latched{false};
  std::atomic<bool> crouch_latched{false};
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
  std::atomic<int> anisotropy{16};
  std::atomic<int> msaa{1};
  std::atomic<int> texture_filter{0}; // 0 default, 1 nearest, 2 linear
  std::atomic<bool> true_color{true};
  std::atomic<bool> disable_copy_filter{false};

  // Reversible post-processing stack.  Off means Dolphin receives the exact
  // same post-process selection that was active before the MOHF PC layer.
  std::atomic<bool> enhanced_graphics{false};
  std::atomic<int> enhanced_preset{1}; // 0 custom/original, 1 enhanced, 2 remastered
  std::atomic<bool> gfx_bloom{true};
  std::atomic<float> gfx_bloom_intensity{0.55f};
  std::atomic<float> gfx_bloom_threshold{0.72f};
  std::atomic<bool> gfx_tonemap{true};
  std::atomic<float> gfx_exposure{1.0f};
  std::atomic<float> gfx_contrast{1.04f};
  std::atomic<float> gfx_saturation{1.03f};
  std::atomic<bool> gfx_sharpen{true};
  std::atomic<float> gfx_sharpen_strength{0.22f};
  std::atomic<bool> gfx_dof{false};
  std::atomic<bool> gfx_dof_ads_only{true};
  std::atomic<float> gfx_dof_strength{0.30f};
  std::atomic<bool> gfx_enhanced_lighting{true};
  std::atomic<float> gfx_lighting_strength{0.28f};
  std::atomic<bool> gfx_ssao{true};
  std::atomic<float> gfx_ssao_strength{0.22f};
  std::atomic<bool> gfx_contact_shadows{true};
  std::atomic<float> gfx_contact_shadow_strength{0.18f};
  std::atomic<bool> gfx_vignette{false};
  std::atomic<float> gfx_vignette_strength{0.12f};
  std::atomic<bool> gfx_film_grain{false};
  std::atomic<float> gfx_film_grain_strength{0.015f};
  std::atomic<float> hud_scale{1.0f};
  std::atomic<float> hud_safe_width{1.0f};
  std::atomic<float> gamepad_sensitivity{1.0f};
  std::atomic<float> gamepad_deadzone{0.10f};
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
  std::atomic<bool> debug_open{false};
  std::atomic<float> mobile_move_x{0.0f};
  std::atomic<float> mobile_move_y{0.0f};
  std::atomic<u32> mobile_actions{0};
  std::string settings_path;
  std::string platform_name{"desktop"};
  std::string original_post_shader;
  std::string applied_post_shader;
  std::chrono::steady_clock::time_point ads_last_update{};
  double smooth_x = 0.0;
  double smooth_y = 0.0;
  double adaptive_ema = 1.0;
  int adaptive_recovery_samples = 0;
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
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

void UnsetEnv(const char* name)
{
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
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


bool MobileDown(MobileAction action)
{
  const u32 bit = 1u << static_cast<u32>(action);
  return (s.mobile_actions.load(std::memory_order_relaxed) & bit) != 0;
}

bool AimActive()
{
  const bool pad = s.gamepad_aim.load(std::memory_order_relaxed);
  if (s.toggle_aim.load(std::memory_order_relaxed))
    return s.aim_latched.load(std::memory_order_relaxed) || MobileDown(MobileAction::Aim) || pad;
  const u32 bit = 1u << static_cast<u32>(std::clamp(s.aim_button.load(), 0, 2));
  return (s.mouse_buttons.load(std::memory_order_relaxed) & bit) != 0 ||
         MobileDown(MobileAction::Aim) || pad;
}

double ApplyDeadzone(double value, double deadzone, double sensitivity)
{
  const double a = std::fabs(value);
  if (a <= deadzone)
    return 0.0;
  const double normalized = (a - deadzone) / std::max(1.0 - deadzone, 0.001);
  return std::copysign(std::clamp(normalized * sensitivity, 0.0, 1.0), value);
}

AnisotropicFilteringMode AnisotropyMode(int value)
{
  switch (value)
  {
  case 1: return AnisotropicFilteringMode::Force1x;
  case 2: return AnisotropicFilteringMode::Force2x;
  case 4: return AnisotropicFilteringMode::Force4x;
  case 8: return AnisotropicFilteringMode::Force8x;
  case 16: return AnisotropicFilteringMode::Force16x;
  default: return AnisotropicFilteringMode::Default;
  }
}

TextureFilteringMode TextureMode(int value)
{
  switch (value)
  {
  case 1: return TextureFilteringMode::Nearest;
  case 2: return TextureFilteringMode::Linear;
  default: return TextureFilteringMode::Default;
  }
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

void ApplyAdsEnvironment()
{
  if (s.fps_ads_enabled.load())
    SetEnv("MOH_CAMERA_PATCH", "1");
  SetEnv("MOH_FPS_ADS", s.fps_ads_enabled.load() ? "1" : "0");
  SetEnv("MOH_ADS_WORLD_FOV", std::to_string(s.ads_world_fov.load()));
  SetEnv("MOH_ADS_WEAPON_FOV", std::to_string(s.ads_weapon_fov.load()));
}

void SetPostOptionBool(VideoCommon::PostProcessingConfiguration* config, const char* name, bool value)
{
  if (config && config->GetOptions().contains(name))
    config->SetOptionb(name, value);
}

void SetPostOptionFloat(VideoCommon::PostProcessingConfiguration* config, const char* name, float value)
{
  if (config && config->GetOptions().contains(name))
    config->SetOptionf(name, 0, value);
}

void ApplyEnhancedPreset(int preset)
{
  if (preset == 1) // Enhanced: restrained preservation-friendly defaults.
  {
    s.gfx_bloom = true; s.gfx_bloom_intensity = 0.55f; s.gfx_bloom_threshold = 0.72f;
    s.gfx_tonemap = true; s.gfx_exposure = 1.0f; s.gfx_contrast = 1.04f; s.gfx_saturation = 1.03f;
    s.gfx_sharpen = true; s.gfx_sharpen_strength = 0.22f;
    s.gfx_dof = false; s.gfx_dof_strength = 0.25f;
    s.gfx_enhanced_lighting = true; s.gfx_lighting_strength = 0.28f;
    s.gfx_ssao = true; s.gfx_ssao_strength = 0.22f;
    s.gfx_contact_shadows = true; s.gfx_contact_shadow_strength = 0.18f;
    s.gfx_vignette = false; s.gfx_film_grain = false;
  }
  else if (preset == 2) // Remastered: intentionally stronger, still reversible.
  {
    s.gfx_bloom = true; s.gfx_bloom_intensity = 0.78f; s.gfx_bloom_threshold = 0.62f;
    s.gfx_tonemap = true; s.gfx_exposure = 1.04f; s.gfx_contrast = 1.08f; s.gfx_saturation = 1.06f;
    s.gfx_sharpen = true; s.gfx_sharpen_strength = 0.32f;
    s.gfx_dof = true; s.gfx_dof_strength = 0.34f;
    s.gfx_enhanced_lighting = true; s.gfx_lighting_strength = 0.42f;
    s.gfx_ssao = true; s.gfx_ssao_strength = 0.34f;
    s.gfx_contact_shadows = true; s.gfx_contact_shadow_strength = 0.28f;
    s.gfx_vignette = true; s.gfx_vignette_strength = 0.10f;
    s.gfx_film_grain = false;
  }
}

void UpdateAdsAnimation()
{
  const auto now = std::chrono::steady_clock::now();
  if (s.ads_last_update.time_since_epoch().count() == 0)
  {
    s.ads_last_update = now;
    return;
  }
  const float dt = static_cast<float>(std::clamp(
      std::chrono::duration<double>(now - s.ads_last_update).count(), 0.0, 0.05));
  s.ads_last_update = now;
  const bool active = s.fps_ads_enabled.load() && s.gameplay.load() && AimActive() &&
                      !s.settings_open.load();
  const float target = active ? 1.0f : 0.0f;
  const float duration = std::max(s.ads_transition_ms.load() * 0.001f, 0.025f);
  float blend = s.ads_blend.load();
  const float step = std::clamp(dt / duration, 0.0f, 1.0f);
  blend += (target - blend) * step;
  if (std::fabs(target - blend) < 0.001f)
    blend = target;
  s.ads_blend = std::clamp(blend, 0.0f, 1.0f);
}

void UpdateEnhancedPostProcess()
{
  const bool enabled = s.enhanced_graphics.load();
  const std::string desired = enabled ? "MOHFrontlineEnhanced" : s.original_post_shader;
  if (s.applied_post_shader != desired)
  {
    Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, desired);
    s.applied_post_shader = desired;
    std::fprintf(stderr, "[moh-gfx] enhanced graphics %s (shader=%s)\n",
                 enabled ? "ON" : "OFF", desired.empty() ? "original" : desired.c_str());
  }

  if (!enabled || !g_presenter || !g_presenter->GetPostProcessor())
    return;
  auto* config = g_presenter->GetPostProcessor()->GetConfig();
  if (!config || config->GetShader() != "MOHFrontlineEnhanced")
    return;

  SetPostOptionBool(config, "MASTER_ENABLE", true);
  SetPostOptionBool(config, "BLOOM_ENABLE", s.gfx_bloom.load());
  SetPostOptionFloat(config, "BLOOM_INTENSITY", s.gfx_bloom_intensity.load());
  SetPostOptionFloat(config, "BLOOM_THRESHOLD", s.gfx_bloom_threshold.load());
  SetPostOptionBool(config, "TONEMAP_ENABLE", s.gfx_tonemap.load());
  SetPostOptionFloat(config, "EXPOSURE", s.gfx_exposure.load());
  SetPostOptionFloat(config, "CONTRAST", s.gfx_contrast.load());
  SetPostOptionFloat(config, "SATURATION", s.gfx_saturation.load());
  SetPostOptionBool(config, "SHARPEN_ENABLE", s.gfx_sharpen.load());
  SetPostOptionFloat(config, "SHARPEN_STRENGTH", s.gfx_sharpen_strength.load());
  SetPostOptionBool(config, "DOF_ENABLE", s.gfx_dof.load());
  const float dof = s.gfx_dof_ads_only.load() ?
                        s.gfx_dof_strength.load() * s.ads_blend.load() :
                        s.gfx_dof_strength.load();
  SetPostOptionFloat(config, "DOF_STRENGTH", dof);
  SetPostOptionBool(config, "LIGHTING_ENABLE", s.gfx_enhanced_lighting.load());
  SetPostOptionFloat(config, "LIGHTING_STRENGTH", s.gfx_lighting_strength.load());
  SetPostOptionBool(config, "SSAO_ENABLE", s.gfx_ssao.load());
  SetPostOptionFloat(config, "SSAO_STRENGTH", s.gfx_ssao_strength.load());
  SetPostOptionBool(config, "CONTACT_SHADOW_ENABLE", s.gfx_contact_shadows.load());
  SetPostOptionFloat(config, "CONTACT_SHADOW_STRENGTH", s.gfx_contact_shadow_strength.load());
  SetPostOptionBool(config, "VIGNETTE_ENABLE", s.gfx_vignette.load());
  SetPostOptionFloat(config, "VIGNETTE_STRENGTH", s.gfx_vignette_strength.load());
  SetPostOptionBool(config, "FILM_GRAIN_ENABLE", s.gfx_film_grain.load());
  SetPostOptionFloat(config, "FILM_GRAIN_STRENGTH", s.gfx_film_grain_strength.load());
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
  SetEnv("MOH_HUD_SCALE", std::to_string(s.hud_scale.load()));
  SetEnv("MOH_HUD_SAFE_WIDTH", std::to_string(s.hud_safe_width.load()));
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
  f << "sensitivity_x=" << s.sensitivity_x.load() << '\n';
  f << "sensitivity_y=" << s.sensitivity_y.load() << '\n';
  f << "ads_sensitivity=" << s.ads_sensitivity.load() << '\n';
  f << "fps_ads=" << s.fps_ads_enabled.load() << '\n';
  f << "ads_world_fov=" << s.ads_world_fov.load() << '\n';
  f << "ads_weapon_fov=" << s.ads_weapon_fov.load() << '\n';
  f << "ads_transition_ms=" << s.ads_transition_ms.load() << '\n';
  f << "ads_hide_crosshair=" << s.ads_hide_crosshair.load() << '\n';
  f << "ads_center_strength=" << s.ads_center_strength.load() << '\n';
  f << "ads_target_x=" << s.ads_target_x.load() << '\n';
  f << "ads_target_y=" << s.ads_target_y.load() << '\n';
  f << "ads_z_offset=" << s.ads_z_offset.load() << '\n';
  f << "mouse_smoothing=" << s.mouse_smoothing.load() << '\n';
  f << "mouse_acceleration=" << s.mouse_acceleration.load() << '\n';
  f << "invert_x=" << s.invert_x.load() << '\n';
  f << "invert_y=" << s.invert_y.load() << '\n';
  f << "toggle_aim=" << s.toggle_aim.load() << '\n';
  f << "toggle_crouch=" << s.toggle_crouch.load() << '\n';
  f << "gamepad=" << s.gamepad_enabled.load() << '\n';
  f << "adaptive_fps=" << s.adaptive_fps.load() << '\n';
  f << "adaptive_profile=" << s.adaptive_profile.load() << '\n';
  f << "ui_safe=" << s.ui_safe.load() << '\n';
  f << "hud_scale=" << s.hud_scale.load() << '\n';
  f << "hud_safe_width=" << s.hud_safe_width.load() << '\n';
  f << "fire_button=" << s.fire_button.load() << '\n';
  f << "aim_button=" << s.aim_button.load() << '\n';
  f << "internal_resolution=" << s.internal_resolution.load() << '\n';
  f << "anisotropy=" << s.anisotropy.load() << '\n';
  f << "msaa=" << s.msaa.load() << '\n';
  f << "texture_filter=" << s.texture_filter.load() << '\n';
  f << "true_color=" << s.true_color.load() << '\n';
  f << "disable_copy_filter=" << s.disable_copy_filter.load() << '\n';
  f << "enhanced_graphics=" << s.enhanced_graphics.load() << '\n';
  f << "enhanced_preset=" << s.enhanced_preset.load() << '\n';
  f << "gfx_bloom=" << s.gfx_bloom.load() << '\n';
  f << "gfx_bloom_intensity=" << s.gfx_bloom_intensity.load() << '\n';
  f << "gfx_bloom_threshold=" << s.gfx_bloom_threshold.load() << '\n';
  f << "gfx_tonemap=" << s.gfx_tonemap.load() << '\n';
  f << "gfx_exposure=" << s.gfx_exposure.load() << '\n';
  f << "gfx_contrast=" << s.gfx_contrast.load() << '\n';
  f << "gfx_saturation=" << s.gfx_saturation.load() << '\n';
  f << "gfx_sharpen=" << s.gfx_sharpen.load() << '\n';
  f << "gfx_sharpen_strength=" << s.gfx_sharpen_strength.load() << '\n';
  f << "gfx_dof=" << s.gfx_dof.load() << '\n';
  f << "gfx_dof_ads_only=" << s.gfx_dof_ads_only.load() << '\n';
  f << "gfx_dof_strength=" << s.gfx_dof_strength.load() << '\n';
  f << "gfx_lighting=" << s.gfx_enhanced_lighting.load() << '\n';
  f << "gfx_lighting_strength=" << s.gfx_lighting_strength.load() << '\n';
  f << "gfx_ssao=" << s.gfx_ssao.load() << '\n';
  f << "gfx_ssao_strength=" << s.gfx_ssao_strength.load() << '\n';
  f << "gfx_contact_shadows=" << s.gfx_contact_shadows.load() << '\n';
  f << "gfx_contact_shadow_strength=" << s.gfx_contact_shadow_strength.load() << '\n';
  f << "gfx_vignette=" << s.gfx_vignette.load() << '\n';
  f << "gfx_vignette_strength=" << s.gfx_vignette_strength.load() << '\n';
  f << "gfx_film_grain=" << s.gfx_film_grain.load() << '\n';
  f << "gfx_film_grain_strength=" << s.gfx_film_grain_strength.load() << '\n';
  f << "gamepad_sensitivity=" << s.gamepad_sensitivity.load() << '\n';
  f << "gamepad_deadzone=" << s.gamepad_deadzone.load() << '\n';
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
    else if (key == "sensitivity_x") s.sensitivity_x = std::clamp(as_f(), 0.10f, 4.0f);
    else if (key == "sensitivity_y") s.sensitivity_y = std::clamp(as_f(), 0.10f, 4.0f);
    else if (key == "ads_sensitivity") s.ads_sensitivity = std::clamp(as_f(), 0.10f, 2.0f);
    else if (key == "fps_ads") s.fps_ads_enabled = as_i() != 0;
    else if (key == "ads_world_fov") s.ads_world_fov = std::clamp(as_f(), 35.0f, 120.0f);
    else if (key == "ads_weapon_fov") s.ads_weapon_fov = std::clamp(as_f(), 35.0f, 120.0f);
    else if (key == "ads_transition_ms") s.ads_transition_ms = std::clamp(as_f(), 40.0f, 600.0f);
    else if (key == "ads_hide_crosshair") s.ads_hide_crosshair = as_i() != 0;
    else if (key == "ads_center_strength") s.ads_center_strength = std::clamp(as_f(), 0.0f, 1.0f);
    else if (key == "ads_target_x") s.ads_target_x = std::clamp(as_f(), -2.0f, 2.0f);
    else if (key == "ads_target_y") s.ads_target_y = std::clamp(as_f(), -2.0f, 2.0f);
    else if (key == "ads_z_offset") s.ads_z_offset = std::clamp(as_f(), -2.0f, 2.0f);
    else if (key == "mouse_smoothing") s.mouse_smoothing = std::clamp(as_f(), 0.0f, 0.95f);
    else if (key == "mouse_acceleration") s.mouse_acceleration = std::clamp(as_f(), 0.0f, 2.0f);
    else if (key == "invert_x") s.invert_x = as_i() != 0;
    else if (key == "invert_y") s.invert_y = as_i() != 0;
    else if (key == "toggle_aim") s.toggle_aim = as_i() != 0;
    else if (key == "toggle_crouch") s.toggle_crouch = as_i() != 0;
    else if (key == "gamepad") s.gamepad_enabled = as_i() != 0;
    else if (key == "adaptive_fps") s.adaptive_fps = as_i() != 0;
    else if (key == "adaptive_profile") s.adaptive_profile = std::clamp(as_i(), 0, 3);
    else if (key == "ui_safe") s.ui_safe = as_i() != 0;
    else if (key == "hud_scale") s.hud_scale = std::clamp(as_f(), 0.50f, 1.50f);
    else if (key == "hud_safe_width") s.hud_safe_width = std::clamp(as_f(), 0.70f, 1.00f);
    else if (key == "fire_button") s.fire_button = std::clamp(as_i(), 0, 2);
    else if (key == "aim_button") s.aim_button = std::clamp(as_i(), 0, 2);
    else if (key == "internal_resolution") s.internal_resolution = std::clamp(as_i(), 1, 8);
    else if (key == "anisotropy") s.anisotropy = as_i();
    else if (key == "msaa") s.msaa = std::clamp(as_i(), 1, 8);
    else if (key == "texture_filter") s.texture_filter = std::clamp(as_i(), 0, 2);
    else if (key == "true_color") s.true_color = as_i() != 0;
    else if (key == "disable_copy_filter") s.disable_copy_filter = as_i() != 0;
    else if (key == "enhanced_graphics") s.enhanced_graphics = as_i() != 0;
    else if (key == "enhanced_preset") s.enhanced_preset = std::clamp(as_i(), 0, 2);
    else if (key == "gfx_bloom") s.gfx_bloom = as_i() != 0;
    else if (key == "gfx_bloom_intensity") s.gfx_bloom_intensity = std::clamp(as_f(), 0.0f, 1.5f);
    else if (key == "gfx_bloom_threshold") s.gfx_bloom_threshold = std::clamp(as_f(), 0.2f, 1.5f);
    else if (key == "gfx_tonemap") s.gfx_tonemap = as_i() != 0;
    else if (key == "gfx_exposure") s.gfx_exposure = std::clamp(as_f(), 0.5f, 2.0f);
    else if (key == "gfx_contrast") s.gfx_contrast = std::clamp(as_f(), 0.7f, 1.4f);
    else if (key == "gfx_saturation") s.gfx_saturation = std::clamp(as_f(), 0.0f, 1.5f);
    else if (key == "gfx_sharpen") s.gfx_sharpen = as_i() != 0;
    else if (key == "gfx_sharpen_strength") s.gfx_sharpen_strength = std::clamp(as_f(), 0.0f, 1.0f);
    else if (key == "gfx_dof") s.gfx_dof = as_i() != 0;
    else if (key == "gfx_dof_ads_only") s.gfx_dof_ads_only = as_i() != 0;
    else if (key == "gfx_dof_strength") s.gfx_dof_strength = std::clamp(as_f(), 0.0f, 1.0f);
    else if (key == "gfx_lighting") s.gfx_enhanced_lighting = as_i() != 0;
    else if (key == "gfx_lighting_strength") s.gfx_lighting_strength = std::clamp(as_f(), 0.0f, 1.0f);
    else if (key == "gfx_ssao") s.gfx_ssao = as_i() != 0;
    else if (key == "gfx_ssao_strength") s.gfx_ssao_strength = std::clamp(as_f(), 0.0f, 1.0f);
    else if (key == "gfx_contact_shadows") s.gfx_contact_shadows = as_i() != 0;
    else if (key == "gfx_contact_shadow_strength") s.gfx_contact_shadow_strength = std::clamp(as_f(), 0.0f, 1.0f);
    else if (key == "gfx_vignette") s.gfx_vignette = as_i() != 0;
    else if (key == "gfx_vignette_strength") s.gfx_vignette_strength = std::clamp(as_f(), 0.0f, 0.8f);
    else if (key == "gfx_film_grain") s.gfx_film_grain = as_i() != 0;
    else if (key == "gfx_film_grain_strength") s.gfx_film_grain_strength = std::clamp(as_f(), 0.0f, 0.15f);
    else if (key == "gamepad_sensitivity") s.gamepad_sensitivity = std::clamp(as_f(), 0.25f, 2.0f);
    else if (key == "gamepad_deadzone") s.gamepad_deadzone = std::clamp(as_f(), 0.0f, 0.50f);
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
    const double mobile_x = std::clamp<double>(s.mobile_move_x.load(), -1.0, 1.0);
    const double mobile_y = std::clamp<double>(s.mobile_move_y.load(), -1.0, 1.0);
    if (std::fabs(mobile_x) > std::fabs(x)) x = mobile_x;
    if (std::fabs(mobile_y) > std::fabs(y)) y = mobile_y;
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
      return static_cast<ControlState>(ApplyDeadzone(original, s.gamepad_deadzone.load(), 1.0));
    }
    if (control == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
    {
      if (y != 0.0 || !keep_pad)
        return static_cast<ControlState>(y);
      return static_cast<ControlState>(ApplyDeadzone(original, s.gamepad_deadzone.load(), 1.0));
    }
  }
  else if (group == GCPad::C_STICK_GROUP)
  {
    // Mouse look is applied directly to MOH's camera-angle accumulators from
    // CPlayerObject::BeginUpdate.  Do not turn mouse deltas into one-frame
    // C-stick velocities here: that made mouse response tiny and frame-rate
    // dependent.  A physical gamepad still uses the original C-stick path.
    if (block_game)
      return static_cast<ControlState>(0.0);
    if (keep_pad)
      return static_cast<ControlState>(ApplyDeadzone(original, s.gamepad_deadzone.load(),
                                                     s.gamepad_sensitivity.load()));
    return static_cast<ControlState>(0.0);
  }
  else if (group == GCPad::BUTTONS_GROUP)
  {
    double pc = 0.0;
    const u32 buttons = s.mouse_buttons.load();
    if (control == GCPad::A_BUTTON)
      pc = ActionDown(Action::Use) || MobileDown(MobileAction::Use) || (!s.gameplay.load() && (buttons & 1u));
    else if (control == GCPad::B_BUTTON)
      pc = ActionDown(Action::Melee) || MobileDown(MobileAction::Melee) || (!s.gameplay.load() && (buttons & 2u));
    else if (control == GCPad::X_BUTTON)
      pc = (s.toggle_crouch.load() ? s.crouch_latched.load() :
            (ActionDown(Action::Crouch) || KeyDown(KEY_CTRL_L) || KeyDown(KEY_CTRL_R))) ||
           MobileDown(MobileAction::Crouch);
    else if (control == GCPad::Y_BUTTON) pc = ActionDown(Action::Jump) || MobileDown(MobileAction::Jump);
    else if (control == GCPad::Z_BUTTON) pc = ActionDown(Action::Reload) || MobileDown(MobileAction::Reload);
    else if (control == GCPad::START_BUTTON) pc = ActionDown(Action::Pause) || MobileDown(MobileAction::Pause);
    else return std::nullopt;
    return combine(pc);
  }
  else if (group == GCPad::TRIGGERS_GROUP)
  {
    const u32 buttons = s.mouse_buttons.load();
    const bool fire = (buttons & (1u << s.fire_button.load())) != 0 || MobileDown(MobileAction::Fire);
    const bool aim = AimActive();
    if (control == GCPad::L_DIGITAL || control == GCPad::L_ANALOG)
    {
      s.gamepad_aim = keep_pad && original > 0.35;
      return combine(aim ? 1.0 : 0.0);
    }
    if (control == GCPad::R_DIGITAL || control == GCPad::R_ANALOG)
      return combine(fire ? 1.0 : 0.0);
  }
  else if (group == GCPad::DPAD_GROUP)
  {
    double pc = 0.0;
    if (control == DIRECTION_UP)
      pc = ActionDown(Action::NextWeapon) || MobileDown(MobileAction::NextWeapon) || ConsumePulse(s.wheel_up);
    else if (control == DIRECTION_DOWN)
      pc = ActionDown(Action::PreviousWeapon) || MobileDown(MobileAction::PreviousWeapon) || ConsumePulse(s.wheel_down);
    else if (control == DIRECTION_LEFT)
      pc = ActionDown(Action::CenterView) || ((s.mouse_buttons.load() & 4u) != 0);
    else if (control == DIRECTION_RIGHT)
      pc = ActionDown(Action::CallHQ) || MobileDown(MobileAction::CallHQ);
    else return std::nullopt;
    return combine(pc);
  }

  return std::nullopt;
}

void ResetDefaults()
{
  s.sensitivity = 1.0f;
  s.sensitivity_x = 1.0f;
  s.sensitivity_y = 1.0f;
  s.ads_sensitivity = 0.80f;
  s.fps_ads_enabled = false;
  s.ads_world_fov = 72.0f;
  s.ads_weapon_fov = 68.0f;
  s.ads_transition_ms = 180.0f;
  s.ads_hide_crosshair = true;
  s.ads_center_strength = 1.0f;
  s.ads_target_x = 0.0f;
  s.ads_target_y = 0.0f;
  s.ads_z_offset = 0.0f;
  s.ads_blend = 0.0f;
  s.mouse_smoothing = 0.0f;
  s.mouse_acceleration = 0.0f;
  s.invert_x = false;
  s.invert_y = false;
  s.toggle_aim = false;
  s.toggle_crouch = false;
  s.aim_latched = false;
  s.crouch_latched = false;
  s.gamepad_enabled = true;
  s.gamepad_sensitivity = 1.0f;
  s.gamepad_deadzone = 0.10f;
  s.adaptive_fps = true;
  s.adaptive_profile = 2;
  s.ui_safe = true;
  s.hud_scale = 1.0f;
  s.hud_safe_width = 1.0f;
  s.fire_button = 0;
  s.aim_button = 1;
  s.internal_resolution = 3;
  s.anisotropy = 16;
  s.msaa = 1;
  s.texture_filter = 0;
  s.true_color = true;
  s.disable_copy_filter = false;
  s.enhanced_graphics = false;
  s.enhanced_preset = 1;
  ApplyEnhancedPreset(1);
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
  s.sensitivity_x = static_cast<float>(std::clamp(EnvDouble("MOH_MOUSE_SENSITIVITY_X", s.sensitivity_x.load()), 0.10, 4.0));
  s.sensitivity_y = static_cast<float>(std::clamp(EnvDouble("MOH_MOUSE_SENSITIVITY_Y", s.sensitivity_y.load()), 0.10, 4.0));
  s.ads_sensitivity = static_cast<float>(std::clamp(EnvDouble("MOH_MOUSE_ADS_SENSITIVITY", s.ads_sensitivity.load()), 0.10, 2.0));
  s.fps_ads_enabled = EnvTrue("MOH_FPS_ADS", s.fps_ads_enabled.load());
  s.ads_world_fov = static_cast<float>(std::clamp(EnvDouble("MOH_ADS_WORLD_FOV", s.ads_world_fov.load()), 35.0, 120.0));
  s.ads_weapon_fov = static_cast<float>(std::clamp(EnvDouble("MOH_ADS_WEAPON_FOV", s.ads_weapon_fov.load()), 35.0, 120.0));
  s.enhanced_graphics = EnvTrue("MOH_ENHANCED_GRAPHICS", s.enhanced_graphics.load());
  s.invert_x = EnvTrue("MOH_MOUSE_INVERT_X", s.invert_x.load());
  s.invert_y = EnvTrue("MOH_MOUSE_INVERT_Y", s.invert_y.load());
  s.ui_safe = EnvTrue("MOH_UI_SAFE", s.ui_safe.load());
  s.hud_scale = static_cast<float>(std::clamp(EnvDouble("MOH_HUD_SCALE", s.hud_scale.load()), 0.50, 1.50));
  s.hud_safe_width = static_cast<float>(std::clamp(EnvDouble("MOH_HUD_SAFE_WIDTH", s.hud_safe_width.load()), 0.70, 1.0));
  if (const char* profile = std::getenv("MOH_ADAPTIVE_PROFILE"))
  {
    const std::string_view v(profile);
    if (v == "off") { s.adaptive_profile = 0; s.adaptive_fps = false; }
    else if (v == "conservative") { s.adaptive_profile = 1; s.adaptive_fps = true; }
    else if (v == "balanced") { s.adaptive_profile = 2; s.adaptive_fps = true; }
    else if (v == "aggressive") { s.adaptive_profile = 3; s.adaptive_fps = true; }
  }

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
  s.original_post_shader = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  s.applied_post_shader = s.original_post_shader;
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
  Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropyMode(s.anisotropy.load()));
  Config::SetBase(Config::GFX_MSAA, static_cast<u32>(s.msaa.load()));
  Config::SetBase(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, TextureMode(s.texture_filter.load()));
  Config::SetBase(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, s.true_color.load());
  Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER, s.disable_copy_filter.load());
  Config::SetBase(Config::MAIN_AUDIO_BUFFER_SIZE, s.audio_buffer_ms.load());
  Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS, s.fill_audio_gaps.load());
  Config::SetBase(Config::MAIN_AUDIO_PRESERVE_PITCH, s.preserve_audio_pitch.load());
  Config::SetBase(Config::MAIN_AUDIO_VOLUME, s.audio_volume.load());
  ApplyFov();
  ApplyAdsEnvironment();
  if (s.aspect_mode.load() != 0)
    ApplyAspect(s.aspect_mode.load());
  if (s.requested_fps.load() >= 0)
    ApplyFPS();
  UpdateEnhancedPostProcess();
}

void SetGameplayActive(bool active)
{
  s.gameplay = active;
  s.rel_x = 0.0;
  s.rel_y = 0.0;
  s.last_abs_x = -1.0;
  s.last_abs_y = -1.0;
  s.smooth_x = 0.0;
  s.smooth_y = 0.0;
  if (!active)
  {
    s.aim_latched = false;
    s.crouch_latched = false;
    s.mobile_move_x = 0.0f;
    s.mobile_move_y = 0.0f;
    s.mobile_actions = 0;
    s.gamepad_aim = false;
    s.ads_blend = 0.0f;
  }
}

bool IsGameplayActive() { return s.gameplay.load(); }
bool IsSettingsOpen() { return s.settings_open.load(); }
bool IsDebugOpen() { return s.debug_open.load(); }
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

void ToggleDebug()
{
  s.debug_open = !s.debug_open.load();
}

void SetPlatformName(const char* platform)
{
  if (platform && *platform)
    s.platform_name = platform;
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
    const u32 crouch_key = s.keys[static_cast<size_t>(Action::Crouch)].load();
    if (s.toggle_crouch.load() &&
        (keysym == crouch_key || keysym == KEY_CTRL_L || keysym == KEY_CTRL_R))
      s.crouch_latched = !s.crouch_latched.load();

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
  if (down && s.toggle_aim.load() && static_cast<int>(button) == s.aim_button.load())
    s.aim_latched = !s.aim_latched.load();
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

  s.rel_x.fetch_add(dx, std::memory_order_relaxed);
  s.rel_y.fetch_add(dy, std::memory_order_relaxed);

  static bool logged = false;
  if (!logged && (dx != 0.0 || dy != 0.0))
  {
    logged = true;
    std::fprintf(stderr,
                 "[moh-pc] native relative mouse events active: first dx=%+.3f dy=%+.3f\n",
                 dx, dy);
  }
}

bool ConsumeMouseLook(float fov_degrees, float* yaw_delta, float* pitch_delta)
{
  if (!yaw_delta || !pitch_delta)
    return false;
  *yaw_delta = 0.0f;
  *pitch_delta = 0.0f;

  if (!WantsRelativeMouse())
    return false;

  const double dx = s.rel_x.exchange(0.0, std::memory_order_relaxed);
  const double dy = s.rel_y.exchange(0.0, std::memory_order_relaxed);
  if (dx == 0.0 && dy == 0.0)
    return false;

  // Match Carnivorous' Medal of Honor: Frontline Mouse Injector math.  Its
  // user sensitivity is divided by 40; our UI exposes a multiplier directly,
  // therefore 1.0 here corresponds to the injector's normal/default gain.
  constexpr double tau = 6.2831853;
  constexpr double crosshair_y = 1.450000048;
  const double sensitivity = std::clamp<double>(s.sensitivity.load(), 0.01, 20.0);
  const double live_fov = s.fov_override.load() ? static_cast<double>(s.fov.load())
                                                : static_cast<double>(fov_degrees);
  const double fov = std::clamp<double>(
      std::isfinite(live_fov) && live_fov > 1.0 ? live_fov : 35.0, 1.0, 179.0);
  const double fov_scale = fov / 35.0;

  double mx = dx;
  double my = dy;
  const double smoothing = std::clamp<double>(s.mouse_smoothing.load(), 0.0, 0.95);
  if (smoothing > 0.0)
  {
    s.smooth_x = s.smooth_x * smoothing + mx * (1.0 - smoothing);
    s.smooth_y = s.smooth_y * smoothing + my * (1.0 - smoothing);
    mx = s.smooth_x;
    my = s.smooth_y;
  }
  else
  {
    s.smooth_x = mx;
    s.smooth_y = my;
  }

  const double speed = std::hypot(mx, my);
  const double acceleration = std::clamp<double>(s.mouse_acceleration.load(), 0.0, 2.0);
  const double accel_gain = 1.0 + acceleration * std::min(speed / 40.0, 3.0);
  const double ads_gain = AimActive() ? std::clamp<double>(s.ads_sensitivity.load(), 0.10, 2.0) : 1.0;
  const double gain_x = sensitivity * s.sensitivity_x.load() * ads_gain * accel_gain;
  const double gain_y = sensitivity * s.sensitivity_y.load() * ads_gain * accel_gain;

  double yaw = -(mx / 10.0) * gain_x / (360.0 / tau) * fov_scale;
  double pitch = (my / 10.0) * gain_y / (90.0 / crosshair_y) * fov_scale;
  if (s.invert_x.load())
    yaw = -yaw;
  if (s.invert_y.load())
    pitch = -pitch;

  // Compositor/focus transitions may queue a very large delta.  Keep a generous
  // one-frame safety bound while preserving normal raw-mouse response.
  yaw = std::clamp(yaw, -1.0, 1.0);
  pitch = std::clamp(pitch, -1.0, 1.0);

  *yaw_delta = static_cast<float>(yaw);
  *pitch_delta = static_cast<float>(pitch);
  return true;
}

void SetMobileMove(float x, float y)
{
  s.mobile_move_x = std::clamp(x, -1.0f, 1.0f);
  s.mobile_move_y = std::clamp(y, -1.0f, 1.0f);
}

void SetMobileAction(MobileAction action, bool down)
{
  const u32 index = static_cast<u32>(action);
  if (index >= static_cast<u32>(MobileAction::Count))
    return;
  const u32 bit = 1u << index;
  u32 mask = s.mobile_actions.load();
  do
  {
    const u32 next = down ? (mask | bit) : (mask & ~bit);
    if (s.mobile_actions.compare_exchange_weak(mask, next))
      break;
  } while (true);
}

void AdaptivePerformanceUpdate()
{
  if (!s.initialized.load() || !s.gameplay.load() || !s.adaptive_fps.load() ||
      s.adaptive_profile.load() == 0 || !std::getenv("MOH_TIMING_PATCH") ||
      !Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE))
    return;

  const int fps = s.requested_fps.load();
  const double target_fps = fps == 0 ? 1000.0 : (fps > 0 ? fps : NATIVE_VPS);
  const double target_factor = std::max(1.0, target_fps / NATIVE_VPS);
  const double current = Config::Get(Config::MAIN_VI_OVERCLOCK);
  const double max_speed = Core::System::GetInstance().GetPerfMetrics().GetMaxSpeed();
  if (!std::isfinite(max_speed) || max_speed <= 0.0)
    return;

  const int profile = std::clamp(s.adaptive_profile.load(), 1, 3);
  const double alpha = profile == 1 ? 0.12 : (profile == 2 ? 0.20 : 0.30);
  const double low = profile == 1 ? 0.99 : (profile == 2 ? 0.985 : 0.975);
  const double high = profile == 1 ? 1.12 : (profile == 2 ? 1.09 : 1.06);
  const double recovery_step = profile == 1 ? 0.020 : (profile == 2 ? 0.035 : 0.055);
  s.adaptive_ema = s.adaptive_ema * (1.0 - alpha) + max_speed * alpha;

  double next = current;
  if (s.adaptive_ema < low)
  {
    s.adaptive_recovery_samples = 0;
    const double pressure = std::clamp(s.adaptive_ema * 0.985, 0.82, 0.995);
    next = std::max(1.0, current * pressure);
  }
  else if (s.adaptive_ema > high && current < target_factor)
  {
    ++s.adaptive_recovery_samples;
    if (s.adaptive_recovery_samples >= (profile == 3 ? 2 : 3))
    {
      next = std::min(target_factor, current + recovery_step);
      s.adaptive_recovery_samples = 0;
    }
  }
  else
  {
    s.adaptive_recovery_samples = 0;
  }

  if (std::fabs(next - current) > 0.010)
  {
    Config::SetCurrent(Config::MAIN_VI_OVERCLOCK, static_cast<float>(next));
    std::fprintf(stderr,
                 "[moh-pc] adaptive FPS/audio: VI %.3f -> %.3f (EMA %.1f%%, instant %.1f%%, ~%.1f FPS)\\n",
                 current, next, s.adaptive_ema * 100.0, max_speed * 100.0,
                 next * NATIVE_VPS);
  }
}

void UpdateFrame()
{
  if (!s.initialized.load())
    return;
  UpdateAdsAnimation();
  UpdateEnhancedPostProcess();
}

float GetAdsBlend() { return s.fps_ads_enabled.load() ? s.ads_blend.load() : 0.0f; }
float GetAdsCenterStrength() { return s.ads_center_strength.load(); }
float GetAdsTargetX() { return s.ads_target_x.load(); }
float GetAdsTargetY() { return s.ads_target_y.load(); }
float GetAdsZOffset() { return s.ads_z_offset.load(); }
bool ShouldHideAdsCrosshair()
{
  return s.fps_ads_enabled.load() && s.ads_hide_crosshair.load() && s.ads_blend.load() > 0.72f;
}
void SetCurrentWeaponType(int type) { s.current_weapon_type = type; }
int GetCurrentWeaponType() { return s.current_weapon_type.load(); }

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
        int adaptive_profile = s.adaptive_profile.load();
        const char* adaptive_items[] = {"Off", "Conservative", "Balanced", "Aggressive"};
        if (ImGui::Combo("Adaptive profile", &adaptive_profile, adaptive_items, 4))
        {
          s.adaptive_profile = adaptive_profile;
          s.adaptive_fps = adaptive_profile != 0;
        }

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
        int aniso = s.anisotropy.load();
        const char* aniso_items[] = {"Default", "1x", "2x", "4x", "8x", "16x"};
        const int aniso_values[] = {0, 1, 2, 4, 8, 16};
        int aniso_idx = 0;
        for (int i = 0; i < 6; ++i) if (aniso_values[i] == aniso) aniso_idx = i;
        if (ImGui::Combo("Anisotropic filtering", &aniso_idx, aniso_items, 6))
        {
          s.anisotropy = aniso_values[aniso_idx];
          Config::SetCurrent(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropyMode(s.anisotropy.load()));
        }
        const char* msaa_items[] = {"Off", "2x", "4x", "8x"};
        const int msaa_values[] = {1, 2, 4, 8};
        int msaa_idx = 0;
        for (int i = 0; i < 4; ++i) if (msaa_values[i] == s.msaa.load()) msaa_idx = i;
        if (ImGui::Combo("MSAA", &msaa_idx, msaa_items, 4))
        {
          s.msaa = msaa_values[msaa_idx];
          Config::SetCurrent(Config::GFX_MSAA, static_cast<u32>(s.msaa.load()));
        }
        const char* filter_items[] = {"Game default", "Nearest", "Linear"};
        int filtering = s.texture_filter.load();
        if (ImGui::Combo("Texture filtering", &filtering, filter_items, 3))
        {
          s.texture_filter = filtering;
          Config::SetCurrent(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, TextureMode(filtering));
        }
        bool true_color = s.true_color.load();
        if (ImGui::Checkbox("Force true color", &true_color))
        {
          s.true_color = true_color;
          Config::SetCurrent(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, true_color);
        }
        bool no_copy_filter = s.disable_copy_filter.load();
        if (ImGui::Checkbox("Disable GameCube copy filter", &no_copy_filter))
        {
          s.disable_copy_filter = no_copy_filter;
          Config::SetCurrent(Config::GFX_ENHANCE_DISABLE_COPY_FILTER, no_copy_filter);
        }
        bool vsync = Config::Get(Config::GFX_VSYNC);
        if (ImGui::Checkbox("VSync", &vsync)) Config::SetCurrent(Config::GFX_VSYNC, vsync);
        ImGui::Text("Actual: %.1f FPS | VI %.1f Hz | Max speed %.0f%%",
                    Core::System::GetInstance().GetPerfMetrics().GetFPS(),
                    Config::Get(Config::MAIN_VI_OVERCLOCK) * NATIVE_VPS,
                    Core::System::GetInstance().GetPerfMetrics().GetMaxSpeed() * 100.0);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Enhanced Graphics"))
      {
        bool enhanced = s.enhanced_graphics.load();
        if (ImGui::Checkbox("Enhanced Graphics (reversible post-process)", &enhanced))
        {
          s.enhanced_graphics = enhanced;
          UpdateEnhancedPostProcess();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(enhanced ? "ON" : "Original renderer");

        int preset = s.enhanced_preset.load();
        const char* presets[] = {"Custom", "Enhanced", "Remastered"};
        if (ImGui::Combo("Preset", &preset, presets, 3))
        {
          s.enhanced_preset = preset;
          if (preset > 0) ApplyEnhancedPreset(preset);
        }
        if (!enhanced)
          ImGui::BeginDisabled();

        ImGui::SeparatorText("Light / atmosphere");
        bool bloom = s.gfx_bloom.load();
        if (ImGui::Checkbox("Bloom", &bloom)) { s.gfx_bloom = bloom; s.enhanced_preset = 0; }
        float bloom_i = s.gfx_bloom_intensity.load(), bloom_t = s.gfx_bloom_threshold.load();
        if (ImGui::SliderFloat("Bloom intensity", &bloom_i, 0.0f, 1.5f, "%.2f")) { s.gfx_bloom_intensity = bloom_i; s.enhanced_preset = 0; }
        if (ImGui::SliderFloat("Bloom threshold", &bloom_t, 0.25f, 1.25f, "%.2f")) { s.gfx_bloom_threshold = bloom_t; s.enhanced_preset = 0; }
        bool lighting = s.gfx_enhanced_lighting.load();
        if (ImGui::Checkbox("Enhanced screen-space lighting", &lighting)) { s.gfx_enhanced_lighting = lighting; s.enhanced_preset = 0; }
        float lighting_i = s.gfx_lighting_strength.load();
        if (ImGui::SliderFloat("Lighting strength", &lighting_i, 0.0f, 1.0f, "%.2f")) { s.gfx_lighting_strength = lighting_i; s.enhanced_preset = 0; }
        bool ssao = s.gfx_ssao.load();
        if (ImGui::Checkbox("Ambient occlusion enhancement", &ssao)) { s.gfx_ssao = ssao; s.enhanced_preset = 0; }
        float ssao_i = s.gfx_ssao_strength.load();
        if (ImGui::SliderFloat("AO strength", &ssao_i, 0.0f, 1.0f, "%.2f")) { s.gfx_ssao_strength = ssao_i; s.enhanced_preset = 0; }
        bool contact = s.gfx_contact_shadows.load();
        if (ImGui::Checkbox("Contact-shadow enhancement", &contact)) { s.gfx_contact_shadows = contact; s.enhanced_preset = 0; }
        float contact_i = s.gfx_contact_shadow_strength.load();
        if (ImGui::SliderFloat("Contact shadow strength", &contact_i, 0.0f, 1.0f, "%.2f")) { s.gfx_contact_shadow_strength = contact_i; s.enhanced_preset = 0; }
        ImGui::TextDisabled("AO/contact shadows are color/depth-cue screen-space enhancements; no game lights are replaced.");

        ImGui::SeparatorText("Image");
        bool tonemap = s.gfx_tonemap.load();
        if (ImGui::Checkbox("Filmic tone mapping", &tonemap)) { s.gfx_tonemap = tonemap; s.enhanced_preset = 0; }
        float exposure=s.gfx_exposure.load(), contrast=s.gfx_contrast.load(), saturation=s.gfx_saturation.load();
        if (ImGui::SliderFloat("Exposure", &exposure, 0.5f, 2.0f, "%.2f")) { s.gfx_exposure=exposure; s.enhanced_preset=0; }
        if (ImGui::SliderFloat("Contrast", &contrast, 0.7f, 1.4f, "%.2f")) { s.gfx_contrast=contrast; s.enhanced_preset=0; }
        if (ImGui::SliderFloat("Saturation", &saturation, 0.0f, 1.5f, "%.2f")) { s.gfx_saturation=saturation; s.enhanced_preset=0; }
        bool sharpen=s.gfx_sharpen.load();
        if (ImGui::Checkbox("Sharpen", &sharpen)) { s.gfx_sharpen=sharpen; s.enhanced_preset=0; }
        float sharpen_i=s.gfx_sharpen_strength.load();
        if (ImGui::SliderFloat("Sharpen strength", &sharpen_i, 0.0f, 1.0f, "%.2f")) { s.gfx_sharpen_strength=sharpen_i; s.enhanced_preset=0; }

        ImGui::SeparatorText("Cinematic effects");
        bool dof=s.gfx_dof.load();
        if (ImGui::Checkbox("Cinematic depth-of-field", &dof)) { s.gfx_dof=dof; s.enhanced_preset=0; }
        bool dof_ads=s.gfx_dof_ads_only.load();
        if (ImGui::Checkbox("DOF only while FPS ADS", &dof_ads)) s.gfx_dof_ads_only=dof_ads;
        float dof_i=s.gfx_dof_strength.load();
        if (ImGui::SliderFloat("DOF strength", &dof_i, 0.0f, 1.0f, "%.2f")) { s.gfx_dof_strength=dof_i; s.enhanced_preset=0; }
        bool vignette=s.gfx_vignette.load();
        if (ImGui::Checkbox("Vignette", &vignette)) { s.gfx_vignette=vignette; s.enhanced_preset=0; }
        float vignette_i=s.gfx_vignette_strength.load();
        if (ImGui::SliderFloat("Vignette strength", &vignette_i, 0.0f, 0.6f, "%.2f")) { s.gfx_vignette_strength=vignette_i; s.enhanced_preset=0; }
        bool grain=s.gfx_film_grain.load();
        if (ImGui::Checkbox("Film grain", &grain)) { s.gfx_film_grain=grain; s.enhanced_preset=0; }
        float grain_i=s.gfx_film_grain_strength.load();
        if (ImGui::SliderFloat("Film grain strength", &grain_i, 0.0f, 0.10f, "%.3f")) { s.gfx_film_grain_strength=grain_i; s.enhanced_preset=0; }

        if (!enhanced)
          ImGui::EndDisabled();
        if (ImGui::Button("Restore preservation-friendly Enhanced preset"))
        {
          s.enhanced_preset = 1;
          ApplyEnhancedPreset(1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Original graphics OFF"))
        {
          s.enhanced_graphics = false;
          UpdateEnhancedPostProcess();
        }
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Aiming"))
      {
        bool fps_ads=s.fps_ads_enabled.load();
        if (ImGui::Checkbox("FPS Aim Down Sight (CoD-style presentation)", &fps_ads))
        {
          s.fps_ads_enabled=fps_ads;
          if (!fps_ads) s.ads_blend=0.0f;
          ApplyAdsEnvironment();
        }
        float ads_world=s.ads_world_fov.load(), ads_weapon=s.ads_weapon_fov.load();
        float transition=s.ads_transition_ms.load();
        if (ImGui::SliderFloat("ADS world FOV", &ads_world, 45.0f, 100.0f, "%.1f deg")) { s.ads_world_fov=ads_world; ApplyAdsEnvironment(); }
        if (ImGui::SliderFloat("ADS weapon FOV", &ads_weapon, 40.0f, 100.0f, "%.1f deg")) { s.ads_weapon_fov=ads_weapon; ApplyAdsEnvironment(); }
        if (ImGui::SliderFloat("ADS transition", &transition, 60.0f, 450.0f, "%.0f ms")) s.ads_transition_ms=transition;
        bool hide=s.ads_hide_crosshair.load();
        if (ImGui::Checkbox("Hide crosshair at full ADS", &hide)) s.ads_hide_crosshair=hide;
        float center=s.ads_center_strength.load();
        if (ImGui::SliderFloat("Iron-sight centering", &center, 0.0f, 1.0f, "%.2f")) s.ads_center_strength=center;
        float tx=s.ads_target_x.load(), ty=s.ads_target_y.load(), tz=s.ads_z_offset.load();
        if (ImGui::SliderFloat("Sight target X", &tx, -1.5f, 1.5f, "%.3f")) s.ads_target_x=tx;
        if (ImGui::SliderFloat("Sight target Y", &ty, -1.5f, 1.5f, "%.3f")) s.ads_target_y=ty;
        if (ImGui::SliderFloat("Sight Z offset", &tz, -1.5f, 1.5f, "%.3f")) s.ads_z_offset=tz;
        ImGui::Text("ADS blend: %.2f | detected weapon type: %d", s.ads_blend.load(), s.current_weapon_type.load());
        ImGui::TextWrapped("The original aim/fire mechanics remain active. FPS ADS only animates camera FOV, viewmodel centering, mouse sensitivity and optional crosshair/DOF presentation. Use the X/Y/Z sliders to fine-calibrate iron sights per build/weapon animation.");
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
        float sens_x = s.sensitivity_x.load();
        float sens_y = s.sensitivity_y.load();
        float ads = s.ads_sensitivity.load();
        if (ImGui::SliderFloat("Mouse X multiplier", &sens_x, 0.25f, 2.0f, "%.2f")) s.sensitivity_x = sens_x;
        if (ImGui::SliderFloat("Mouse Y multiplier", &sens_y, 0.25f, 2.0f, "%.2f")) s.sensitivity_y = sens_y;
        if (ImGui::SliderFloat("ADS sensitivity", &ads, 0.20f, 1.50f, "%.2f")) s.ads_sensitivity = ads;
        float smoothing = s.mouse_smoothing.load();
        float acceleration = s.mouse_acceleration.load();
        if (ImGui::SliderFloat("Mouse smoothing", &smoothing, 0.0f, 0.90f, "%.2f")) s.mouse_smoothing = smoothing;
        if (ImGui::SliderFloat("Mouse acceleration", &acceleration, 0.0f, 2.0f, "%.2f")) s.mouse_acceleration = acceleration;
        bool invert_x = s.invert_x.load();
        bool invert_y = s.invert_y.load();
        if (ImGui::Checkbox("Invert mouse X", &invert_x)) s.invert_x = invert_x;
        ImGui::SameLine();
        if (ImGui::Checkbox("Invert mouse Y", &invert_y)) s.invert_y = invert_y;
        bool toggle_aim = s.toggle_aim.load();
        bool toggle_crouch = s.toggle_crouch.load();
        if (ImGui::Checkbox("Toggle aim", &toggle_aim)) { s.toggle_aim = toggle_aim; s.aim_latched = false; }
        ImGui::SameLine();
        if (ImGui::Checkbox("Toggle crouch", &toggle_crouch)) { s.toggle_crouch = toggle_crouch; s.crouch_latched = false; }
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
        float pad_sens = s.gamepad_sensitivity.load();
        float deadzone = s.gamepad_deadzone.load();
        if (ImGui::SliderFloat("Stick sensitivity", &pad_sens, 0.50f, 2.0f, "%.2f")) s.gamepad_sensitivity = pad_sens;
        if (ImGui::SliderFloat("Stick deadzone", &deadzone, 0.0f, 0.40f, "%.2f")) s.gamepad_deadzone = deadzone;
        ImGui::TextWrapped("The original GameCube pad remains fully supported. Keyboard/mouse is merged on top only when enabled, so you can switch devices at any time.");
        ImGui::TextDisabled("GameCube layout remains authoritative for gamepad prompts and rumble.");
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("HUD / Accessibility"))
      {
        bool safe = s.ui_safe.load();
        if (ImGui::Checkbox("Aspect-correct HUD/menu safe area", &safe))
        {
          s.ui_safe = safe;
          SetEnv("MOH_UI_SAFE", safe ? "1" : "0");
        }
        float hud_scale = s.hud_scale.load();
        float safe_width = s.hud_safe_width.load();
        if (ImGui::SliderFloat("HUD scale", &hud_scale, 0.50f, 1.50f, "%.2fx"))
        {
          s.hud_scale = hud_scale;
          SetEnv("MOH_HUD_SCALE", std::to_string(hud_scale));
        }
        if (ImGui::SliderFloat("HUD safe width", &safe_width, 0.70f, 1.00f, "%.2f"))
        {
          s.hud_safe_width = safe_width;
          SetEnv("MOH_HUD_SAFE_WIDTH", std::to_string(safe_width));
        }
        ImGui::TextWrapped("3D stays Hor+, while the 2D transform keeps HUD/menu geometry proportional on widescreen and ultrawide outputs.");
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
        ImGui::Text("Platform input: %s", s.platform_name.c_str());
        ImGui::Text("Settings file: %s", s.settings_path.empty() ? "(not configured)" : s.settings_path.c_str());
        bool debug = s.debug_open.load();
        if (ImGui::Checkbox("Developer overlay (Ctrl+F8)", &debug)) s.debug_open = debug;
        if (ImGui::Button("Screenshot")) Core::SaveScreenShot();
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

void DrawDebugUI(float backbuffer_scale)
{
  if (!s.debug_open.load())
    return;
  ImGui::SetNextWindowBgAlpha(0.78f);
  ImGui::SetNextWindowPos(ImVec2(12.0f * backbuffer_scale, 12.0f * backbuffer_scale), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("MOHF Recomp Diagnostics", nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
  {
    const auto& perf = Core::System::GetInstance().GetPerfMetrics();
    ImGui::Text("FPS             %7.2f", perf.GetFPS());
    ImGui::Text("Emulation max   %7.1f %%", perf.GetMaxSpeed() * 100.0);
    ImGui::Text("Adaptive EMA    %7.1f %%", s.adaptive_ema * 100.0);
    ImGui::Text("VI              %7.2f Hz", Config::Get(Config::MAIN_VI_OVERCLOCK) * NATIVE_VPS);
    ImGui::Text("Gameplay        %s", s.gameplay.load() ? "yes" : "no");
    ImGui::Text("Mouse capture   %s", WantsRelativeMouse() ? "yes" : "no");
    ImGui::Text("Input platform  %s", s.platform_name.c_str());
    ImGui::Text("FOV             %.1f", s.fov_override.load() ? s.fov.load() : 0.0f);
    ImGui::Text("Aspect          %d:%d", s.aspect_num.load(), s.aspect_den.load());
    ImGui::Text("Internal res    %dx", s.internal_resolution.load());
    ImGui::Text("Audio buffer    %d ms", s.audio_buffer_ms.load());
    ImGui::Text("Enhanced gfx    %s", s.enhanced_graphics.load() ? "on" : "off");
    ImGui::Text("FPS ADS         %s  blend %.2f", s.fps_ads_enabled.load() ? "on" : "off", s.ads_blend.load());
    ImGui::Text("Weapon type     %d", s.current_weapon_type.load());
  }
  ImGui::End();
}

} // namespace MohPcLayer
