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

void DrawSettingsUI(float backbuffer_scale);
}
