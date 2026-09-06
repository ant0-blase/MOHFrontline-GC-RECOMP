#ifndef MOH_FRONTLINE_ENHANCEMENTS_H
#define MOH_FRONTLINE_ENHANCEMENTS_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

int moh_camera_override(CPUState* ctx);
int moh_wide_fov_force_skybox_faces(void);
int moh_weapon_projection_override(CPUState* ctx);
void moh_ui_prepare(CPUState* ctx);
void moh_ui_matrix_override(CPUState* ctx);
void moh_ui_begin(CPUState* ctx);
void moh_ui_end(CPUState* ctx);
void moh_hud_begin(CPUState* ctx);
void moh_hud_end(CPUState* ctx);
void moh_hud_poly_begin(CPUState* ctx);
void moh_hud_poly_end(CPUState* ctx);
void moh_hud_text_position_override(CPUState* ctx);
void moh_hud_centered_text_position_override(CPUState* ctx);
void moh_ui_font_scale_override(CPUState* ctx);
void moh_ps3_formatted_font(CPUState* ctx);
void moh_ps3_metadata(CPUState* ctx, u32 token, u32 name);
/* GMFE69 quality-of-life: allow the M1 Garand to start its original reload
 * sequence with a partially used en-bloc clip. */
void moh_m1_manual_reload_prepare(CPUState* ctx);
void moh_m1_manual_reload_restore(CPUState* ctx);
int moh_timing_enabled(void);
void moh_player_vertical_delta_fix(CPUState* ctx);
void moh_timing_set_gameplay(CPUState* ctx, int active);
void moh_timing_frame_advance(void);
f64 moh_timing_delta_ticks(void);
u32 moh_timing_integer_ticks(void);
f64 moh_timing_game_time_seconds(void);
u32 moh_timing_frame_count(void);
u32 moh_timing_vsyncs(CPUState* ctx);
void moh_timing_arm_present(CPUState* ctx);

#ifdef __cplusplus
}
#endif

#endif
