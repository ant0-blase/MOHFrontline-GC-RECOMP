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
void UpdateFrame();

// GMFE69 direct-present bridge: arm exactly the final XFB copied by CScreen::Flip.
void ArmGameplayPresent(u32 xfb_addr);
bool ConsumeGameplayPresent(u32 xfb_addr);
bool ConsumeVBlankPresentSuppression(u32 xfb_addr);

void SetGameplayActive(bool active);
bool IsGameplayActive();
void SetMovieActive(bool active);
bool IsSettingsOpen();
bool IsDebugOpen();

// Enhanced scene compositor.
// The user post-process is baked into gameplay 3D before the guest HUD.
// Authoritative GameCube gameplay-state signal.
void SetGameplayDetected(bool active);

void PreprocessSceneBefore2D();

// Final XFB policy.
// true = use Dolphin's default final-copy pipeline.
bool ShouldBypassFinalPostProcess();
void NotifyFinalPostProcessComplete();

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

// Native FPS ADS state queried by the static-recomp host-call bridge.
float GetAdsBlend();
float GetAdsCenterStrength();
float GetAdsTargetX();
float GetAdsTargetY();
float GetAdsZOffset();
bool ShouldHideAdsCrosshair();
bool IsPcCrosshairEnabled();
void SetCurrentWeaponType(int type);
int GetCurrentWeaponType();

// Native PS3 SFNH/CFont renderer.
bool IsPS3FontBridgeReady();
bool QueuePS3FontDraw(const char* text, float x, float y, bool centered);
void DrawPS3FontUI(float backbuffer_scale);

void DrawCrosshair(float backbuffer_scale);
void DrawSettingsUI(float backbuffer_scale);
void DrawDebugUI(float backbuffer_scale);
}
