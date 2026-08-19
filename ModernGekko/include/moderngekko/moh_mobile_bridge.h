#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum MohMobileAction
{
  MOH_MOBILE_FIRE = 0,
  MOH_MOBILE_AIM,
  MOH_MOBILE_USE,
  MOH_MOBILE_RELOAD,
  MOH_MOBILE_JUMP,
  MOH_MOBILE_CROUCH,
  MOH_MOBILE_MELEE,
  MOH_MOBILE_PREVIOUS_WEAPON,
  MOH_MOBILE_NEXT_WEAPON,
  MOH_MOBILE_PAUSE,
  MOH_MOBILE_CALL_HQ,
};

void moh_mobile_set_viewport(int width, int height);
void moh_mobile_set_move(float x, float y);
void moh_mobile_touch_look(float dx, float dy);
void moh_mobile_touch_action(int action, int down);
void moh_mobile_toggle_settings(void);
void moh_mobile_toggle_debug(void);

#ifdef __cplusplus
}
#endif
