#ifndef MOH_FRONTLINE_ENHANCEMENTS_H
#define MOH_FRONTLINE_ENHANCEMENTS_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

int moh_camera_override(CPUState* ctx);
int moh_timing_enabled(void);
int moh_timing_wait(CPUState* ctx);
f64 moh_timing_delta_ticks(void);
u32 moh_timing_integer_ticks(void);
f64 moh_timing_game_time_seconds(void);
u32 moh_timing_frame_count(void);
u32 moh_timing_vsyncs(CPUState* ctx);

#ifdef __cplusplus
}
#endif

#endif
