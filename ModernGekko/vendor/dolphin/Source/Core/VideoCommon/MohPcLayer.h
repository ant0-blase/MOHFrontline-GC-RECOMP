// Modern PC layer for Medal of Honor: Frontline (GMFE69).
#pragma once

#include "Common/CommonTypes.h"

namespace MohPcLayer
{
void Initialize();
void Shutdown();
void ApplyDolphinSettings();
void AdaptivePerformanceUpdate();

void SetGameplayActive(bool active);
bool IsGameplayActive();
bool IsSettingsOpen();
bool WantsRelativeMouse();
void ToggleSettings();

void SetWindowSize(int width, int height);
void KeyEvent(u32 keysym, bool down);
void PointerAbsolute(double x, double y);
void PointerButton(unsigned button, bool down);
void PointerAxis(double vertical_steps);
void RelativeMotion(double dx, double dy);
// Consume accumulated raw mouse motion and convert it with the same angular
// calibration used by Carnivorous' Dolphin Mouse Injector driver for MOHF.
// `fov_degrees` is the live player FOV stored by the game.
bool ConsumeMouseLook(float fov_degrees, float* yaw_delta, float* pitch_delta);

void DrawSettingsUI(float backbuffer_scale);
}
