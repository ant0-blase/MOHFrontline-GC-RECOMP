#define _POSIX_C_SOURCE 200809L

#include "moh_frontline_enhancements.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOH_NATIVE_ASPECT (4.0 / 3.0)
#define MOH_PI 3.14159265358979323846264338327950288

static int s_config_ready;
static int s_camera_enabled;
static int s_timing_enabled;
static int s_unlimited;
static double s_aspect = MOH_NATIVE_ASPECT;
static double s_fov_degrees;
static double s_target_fps;

static int s_camera_logged;
static int s_timing_logged;

static u64 s_last_release_ns;
static u64 s_next_release_ns;
static double s_delta_ticks = 1.0;
static double s_game_time_seconds;
static double s_integer_tick_remainder;
static u32 s_integer_ticks = 1;
static int s_vsync_seeded;
static u32 s_vsync_seed;

static int env_enabled(const char* name)
{
    const char* value = getenv(name);
    return value && *value && strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

static double env_double(const char* name, double fallback)
{
    const char* value = getenv(name);
    char* end = NULL;
    double parsed;

    if (!value || !*value)
        return fallback;
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || !isfinite(parsed))
        return fallback;
    return parsed;
}

static __attribute__((noinline, cold)) void load_config_slow(void)
{
    const char* fps;

    if (s_config_ready)
        return;
    s_config_ready = 1;

    s_camera_enabled = env_enabled("MOH_CAMERA_PATCH");
    s_timing_enabled = env_enabled("MOH_TIMING_PATCH");
    s_aspect = env_double("MOH_ASPECT_VALUE", MOH_NATIVE_ASPECT);
    s_fov_degrees = env_double("MOH_FOV_DEGREES", 0.0);

    if (!(s_aspect > 0.5 && s_aspect < 10.0))
        s_aspect = MOH_NATIVE_ASPECT;
    if (!(s_fov_degrees >= 0.0 && s_fov_degrees < 179.0))
        s_fov_degrees = 0.0;

    fps = getenv("MOH_FPS_TARGET");
    if (fps && strcmp(fps, "unlimited") == 0)
    {
        s_unlimited = 1;
        s_target_fps = 0.0;
    }
    else
    {
        s_target_fps = env_double("MOH_FPS_TARGET", 60.0);
        if (!(s_target_fps >= 1.0 && s_target_fps <= 1000.0))
            s_target_fps = 60.0;
    }
}

/* MOH_CONFIG_FASTPATH_BEGIN */
/*
 * Hot-path wrapper: after the first configuration parse, repeated timing/camera
 * calls pay only one predictable load/branch instead of calling the large
 * getenv/strtod/strcmp parser function.
 */
static inline __attribute__((always_inline)) void load_config(void)
{
    if (__builtin_expect(s_config_ready != 0, 1))
        return;
    load_config_slow();
}
/* MOH_CONFIG_FASTPATH_END */


static u64 monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static u32 f32_bits(f32 value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int moh_camera_override(CPUState* ctx)
{
    const u32 camera = ctx->gpr[3];
    const double original_half_x = ctx->fpr[1];
    const double original_half_y = ctx->fpr[2];
    double tan_half_x;
    double tan_half_y;

    load_config();
    if (!s_camera_enabled)
        return 0;

    if (s_fov_degrees > 0.0)
    {
        const double half_x = s_fov_degrees * (MOH_PI / 360.0);
        tan_half_x = tan(half_x);
        tan_half_y = tan_half_x / s_aspect;
    }
    else if (fabs(s_aspect - MOH_NATIVE_ASPECT) < 0.000001)
    {
        tan_half_x = tan(original_half_x);
        tan_half_y = tan(original_half_y);
    }
    else
    {
        /* Hor+: keep the original vertical projection untouched and scale the
         * original horizontal tangent by target/native aspect.  This preserves
         * the exact 4:3 camera calibration used by the game instead of
         * rebuilding it from the legacy fovY=fovX*0.75 approximation. */
        tan_half_x = tan(original_half_x) * (s_aspect / MOH_NATIVE_ASPECT);
        tan_half_y = tan(original_half_y);
    }

    mem_write32(ctx, camera + 44u, f32_bits((f32)tan_half_x));
    mem_write32(ctx, camera + 48u, f32_bits((f32)tan_half_y));
    mem_write8(ctx, camera + 322u, 0u);
    mem_write8(ctx, camera + 323u, 0u);
    mem_write8(ctx, camera + 320u, 1u);

    if (!s_camera_logged)
    {
        s_camera_logged = 1;
        fprintf(stderr,
                "[moh-enh] native camera override active: aspect=%.6f fov=%s%.2f\n",
                s_aspect, s_fov_degrees > 0.0 ? "" : "Hor+ ",
                s_fov_degrees > 0.0 ? s_fov_degrees : 0.0);
    }
    return 1;
}

int moh_timing_enabled(void)
{
    load_config();
    return s_timing_enabled;
}

int moh_timing_wait(CPUState* ctx)
{
    u64 now;
    u64 elapsed_ns;
    u64 target_ns = 0;

    load_config();
    if (!s_timing_enabled)
        return 0;

    now = monotonic_ns();
    if (!now)
    {
        s_delta_ticks = (!s_unlimited && s_target_fps > 0.0) ? 60.0 / s_target_fps : 1.0;
        s_game_time_seconds += s_delta_ticks / 60.0;
        s_integer_tick_remainder += s_delta_ticks;
        s_integer_ticks = (u32)floor(s_integer_tick_remainder);
        s_integer_tick_remainder -= (double)s_integer_ticks;
        return 1;
    }

    if (!s_unlimited && s_target_fps > 0.0)
        target_ns = (u64)(1000000000.0 / s_target_fps);

    if (!s_last_release_ns)
    {
        s_last_release_ns = now;
        s_next_release_ns = target_ns ? now + target_ns : now;
        s_delta_ticks = target_ns ? 60.0 / s_target_fps : 1.0;
    }
    else if (target_ns && now < s_next_release_ns)
    {
        /* Never sleep the emulated CPU thread. Return to the StaticRecomp
         * chassis after one normal generated-loop budget so CoreTiming/VBlank
         * can run, then test the host deadline again. */
        ctx->downcount -= 256;
        return -1;
    }
    else
    {
        elapsed_ns = now > s_last_release_ns ? now - s_last_release_ns : 1u;
        s_last_release_ns = now;

        if (target_ns)
        {
            if (now > s_next_release_ns + target_ns * 4u)
                s_next_release_ns = now + target_ns;
            else
            {
                do
                    s_next_release_ns += target_ns;
                while (s_next_release_ns <= now);
            }
        }

        if (elapsed_ns < 100000ull)
            elapsed_ns = 100000ull;
        if (elapsed_ns > 100000000ull)
            elapsed_ns = 100000000ull;
        s_delta_ticks = ((double)elapsed_ns / 1000000000.0) * 60.0;
    }

    if (s_delta_ticks < 0.001)
        s_delta_ticks = 0.001;
    if (s_delta_ticks > 6.0)
        s_delta_ticks = 6.0;

    s_game_time_seconds += s_delta_ticks / 60.0;
    s_integer_tick_remainder += s_delta_ticks;
    s_integer_ticks = (u32)floor(s_integer_tick_remainder);
    s_integer_tick_remainder -= (double)s_integer_ticks;

    if (!s_timing_logged)
    {
        s_timing_logged = 1;
        if (s_unlimited)
            fprintf(stderr, "[moh-enh] native timing override active: FPS=unlimited (nonblocking)\n");
        else
            fprintf(stderr, "[moh-enh] native timing override active: FPS=%.3f (nonblocking)\n", s_target_fps);
    }

    return 1;
}

f64 moh_timing_delta_ticks(void)
{
    return s_delta_ticks;
}

u32 moh_timing_integer_ticks(void)
{
    return s_integer_ticks;
}

f64 moh_timing_game_time_seconds(void)
{
    return s_game_time_seconds;
}

u32 moh_timing_frame_count(void)
{
    double ticks = floor(s_game_time_seconds * 60.0);
    if (ticks < 0.0)
        return 0;
    if (ticks > 4294967295.0)
        return 0xffffffffu;
    return (u32)ticks;
}

u32 moh_timing_vsyncs(CPUState* ctx)
{
    load_config();
    if (!s_timing_enabled)
        return mem_read32(ctx, ctx->gpr[13] + (u32)(s32)(-29256));

    if (!s_vsync_seeded)
    {
        s_vsync_seed = mem_read32(ctx, ctx->gpr[13] + (u32)(s32)(-29256));
        s_vsync_seeded = 1;
    }
    return s_vsync_seed + moh_timing_frame_count();
}
