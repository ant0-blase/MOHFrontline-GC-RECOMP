#ifndef MOH_FRONTLINE_ENHANCEMENTS_H
#define MOH_FRONTLINE_ENHANCEMENTS_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

int moh_camera_override(CPUState* ctx);
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
