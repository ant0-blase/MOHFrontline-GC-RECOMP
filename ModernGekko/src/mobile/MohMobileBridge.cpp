#include "moderngekko/moh_mobile_bridge.h"

#include "VideoCommon/MohPcLayer.h"

#include <algorithm>

namespace
{
void MarkMobilePlatform()
{
#if defined(__ANDROID__)
  MohPcLayer::SetPlatformName("Android/touch");
#elif defined(__APPLE__)
  MohPcLayer::SetPlatformName("iOS/touch");
#else
  MohPcLayer::SetPlatformName("mobile bridge");
#endif
}
}

extern "C" void moh_mobile_set_viewport(int width, int height)
{
  MarkMobilePlatform();
  MohPcLayer::SetWindowSize(std::max(width, 1), std::max(height, 1));
}

extern "C" void moh_mobile_set_move(float x, float y)
{
  MarkMobilePlatform();
  MohPcLayer::SetMobileMove(x, y);
}

extern "C" void moh_mobile_touch_look(float dx, float dy)
{
  MarkMobilePlatform();
  MohPcLayer::RelativeMotion(dx, dy);
}

extern "C" void moh_mobile_touch_action(int action, int down)
{
  if (action < 0 || action >= static_cast<int>(MohPcLayer::MobileAction::Count))
    return;
  MohPcLayer::SetMobileAction(static_cast<MohPcLayer::MobileAction>(action), down != 0);
}

extern "C" void moh_mobile_toggle_settings(void)
{
  MohPcLayer::ToggleSettings();
}

extern "C" void moh_mobile_toggle_debug(void)
{
  MohPcLayer::ToggleDebug();
}
