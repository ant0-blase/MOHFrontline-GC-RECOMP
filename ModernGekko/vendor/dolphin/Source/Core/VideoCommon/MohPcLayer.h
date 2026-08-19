// Modern PC/portable layer for Medal of Honor: Frontline (GMFE69).
#pragma once

#include "Common/CommonTypes.h"

namespace MohPcLayer
{
enum class MobileAction : u32
{
  Fire = 0,
  Aim,
  Use,
  Reload,
  Jump,
  Crouch,
  Melee,
  PreviousWeapon,
  NextWeapon,
  Pause,
  CallHQ,
  Count,
};

void Initialize();
void Shutdown();
void ApplyDolphinSettings();
void AdaptivePerformanceUpdate();

void SetGameplayActive(bool active);
bool IsGameplayActive();
bool IsSettingsOpen();
bool IsDebugOpen();
bool WantsRelativeMouse();
void ToggleSettings();
void ToggleDebug();
void SetPlatformName(const char* platform);

void SetWindowSize(int width, int height);
void KeyEvent(u32 keysym, bool down);
void PointerAbsolute(double x, double y);
void PointerButton(unsigned button, bool down);
void PointerAxis(double vertical_steps);
void RelativeMotion(double dx, double dy);

// Mobile frontends feed logical movement/look/actions through this API.  The
// frontend never needs to know GameCube pad numbers or MOHF memory offsets.
void SetMobileMove(float x, float y);
void SetMobileAction(MobileAction action, bool down);

// Consume accumulated raw/touch motion and convert it using the native MOHF
// camera calibration. `fov_degrees` is the live player FOV stored by the game.
bool ConsumeMouseLook(float fov_degrees, float* yaw_delta, float* pitch_delta);

void DrawSettingsUI(float backbuffer_scale);
void DrawDebugUI(float backbuffer_scale);
}
