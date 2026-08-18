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

static int s_camera_enabled;
static int s_timing_enabled;
static int s_ui_safe;
static int s_gameplay_active;
static int s_unlimited;
static double s_aspect = MOH_NATIVE_ASPECT;
static double s_fov_degrees;
static double s_weapon_fov_degrees = -1.0;
static double s_target_fps;

static int s_camera_logged;
static int s_weapon_logged;
static int s_timing_logged;

static u64 s_last_frame_ns;
static double s_delta_ticks = 1.0;
static double s_game_time_seconds;
static double s_integer_tick_remainder;
static double s_virtual_ticks_elapsed;
static u32 s_integer_ticks = 1;
static u32 s_game_frame_base;
static u32 s_vsync_seed;
static CPUState* s_gameplay_ctx;
static double s_ui_applied_aspect = -1.0;
static int s_ui_applied_safe = -1;
static int s_ui_logged;

#define MOH_HOSTCALL_VI_GAMEPLAY_ON  0xFFFFF100u
#define MOH_HOSTCALL_VI_GAMEPLAY_OFF 0xFFFFF101u
#define MOH_HOSTCALL_GAMEPLAY_ENTER   0xFFFFF110u
#define MOH_HOSTCALL_GAMEPLAY_EXIT    0xFFFFF111u

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

static void refresh_config(void)
{
    const char* fps;
    const int old_timing = s_timing_enabled;

    s_camera_enabled = env_enabled("MOH_CAMERA_PATCH");
    s_timing_enabled = env_enabled("MOH_TIMING_PATCH");
    s_ui_safe = env_enabled("MOH_UI_SAFE");
    s_aspect = env_double("MOH_ASPECT_VALUE", MOH_NATIVE_ASPECT);
    s_fov_degrees = env_double("MOH_FOV_DEGREES", 0.0);
    s_weapon_fov_degrees = env_double("MOH_WEAPON_FOV_DEGREES", -1.0);

    if (!(s_aspect > 0.5 && s_aspect < 10.0))
        s_aspect = MOH_NATIVE_ASPECT;
    if (!(s_fov_degrees >= 0.0 && s_fov_degrees < 179.0))
        s_fov_degrees = 0.0;
    if (!(s_weapon_fov_degrees >= 20.0 && s_weapon_fov_degrees < 179.0))
        s_weapon_fov_degrees = -1.0;

    s_unlimited = 0;
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

    /* Live settings menu can arm/disarm FPS while a level is already running. */
    if (s_gameplay_active && s_gameplay_ctx && old_timing != s_timing_enabled)
    {
        if (s_timing_enabled)
        {
            if (s_gameplay_ctx->host_call &&
                s_gameplay_ctx->host_call(s_gameplay_ctx, MOH_HOSTCALL_VI_GAMEPLAY_ON))
            {
                s_last_frame_ns = 0;
                s_delta_ticks = 1.0;
                s_integer_tick_remainder = 0.0;
                s_integer_ticks = 0;
            }
            else
            {
                s_timing_enabled = 0;
                s_delta_ticks = 1.0;
            }
        }
        else
        {
            if (s_gameplay_ctx->host_call)
                (void)s_gameplay_ctx->host_call(s_gameplay_ctx, MOH_HOSTCALL_VI_GAMEPLAY_OFF);
            s_last_frame_ns = 0;
            s_delta_ticks = 1.0;
            s_integer_ticks = 1;
        }
    }
}

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

static f32 f32_from_bits(u32 bits)
{
    f32 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

int moh_camera_override(CPUState* ctx)
{
    const u32 camera = ctx->gpr[3];
    const double original_half_x = ctx->fpr[1];
    const double original_half_y = ctx->fpr[2];
    double tan_half_x;
    double tan_half_y;

    refresh_config();
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

int moh_weapon_projection_override(CPUState* ctx)
{
    double horizontal_scale = 1.0;
    double vertical_scale = 1.0;
    double requested_fov;

    refresh_config();
    if (!s_camera_enabled)
        return 0;

    /* By default the weapon follows --fov.  --weapon-fov can override it.
     * An explicit FOV is interpreted as the final horizontal viewmodel FOV,
     * just like --fov is for the world camera.  Scale both weapon tangents by
     * the same amount so the gun itself keeps its proportions. */
    requested_fov = s_weapon_fov_degrees > 0.0 ? s_weapon_fov_degrees : s_fov_degrees;
    if (requested_fov > 0.0)
    {
        const double original_half = 35.0 * (MOH_PI / 180.0);
        const double requested_half = requested_fov * (MOH_PI / 360.0);
        const double fov_scale = tan(requested_half) / tan(original_half);
        horizontal_scale = fov_scale;
        vertical_scale = fov_scale;
    }
    else if (fabs(s_aspect - MOH_NATIVE_ASPECT) >= 0.000001)
    {
        /* Aspect-only mode is Hor+: widen only the horizontal tangent so the
         * viewmodel follows the world camera without becoming taller/shorter. */
        horizontal_scale = s_aspect / MOH_NATIVE_ASPECT;
    }

    ctx->fpr[1] *= horizontal_scale;
    ctx->fpr[2] *= vertical_scale;

    if (!s_weapon_logged)
    {
        s_weapon_logged = 1;
        if (requested_fov > 0.0)
            fprintf(stderr,
                    "[moh-enh] weapon FOV override active: %.2f deg (scale %.6f)\n",
                    requested_fov, horizontal_scale);
        else
            fprintf(stderr,
                    "[moh-enh] weapon Hor+ override active: aspect=%.6f (x scale %.6f)\n",
                    s_aspect, horizontal_scale);
    }

    return 1;
}

void moh_ui_prepare(CPUState* ctx)
{
    const u32 screen = ctx->gpr[3];
    refresh_config();
    if (s_ui_applied_safe != s_ui_safe || fabs(s_ui_applied_aspect - s_aspect) > 0.000001)
        mem_write8(ctx, screen + 160u, 0u);
}

void moh_ui_matrix_override(CPUState* ctx)
{
    const u32 screen = ctx->gpr[31];
    const u32 matrix = screen + 96u;
    double scale = 1.0;
    unsigned offsets[3];
    int i;

    refresh_config();
    if (s_ui_safe && fabs(s_aspect - MOH_NATIVE_ASPECT) > 0.000001)
    {
        if (s_aspect > MOH_NATIVE_ASPECT)
        {
            scale = MOH_NATIVE_ASPECT / s_aspect;
            offsets[0] = 0u; offsets[1] = 4u; offsets[2] = 8u;
        }
        else
        {
            scale = s_aspect / MOH_NATIVE_ASPECT;
            offsets[0] = 16u; offsets[1] = 20u; offsets[2] = 24u;
        }
        for (i = 0; i < 3; ++i)
        {
            const f32 old_value = f32_from_bits(mem_read32(ctx, matrix + offsets[i]));
            mem_write32(ctx, matrix + offsets[i], f32_bits((f32)(old_value * scale)));
        }
        if (!s_ui_logged)
        {
            s_ui_logged = 1;
            fprintf(stderr,
                    "[moh-enh] aspect-correct 2D UI active: aspect=%.6f safe-scale=%.6f\n",
                    s_aspect, scale);
        }
    }
    s_ui_applied_aspect = s_aspect;
    s_ui_applied_safe = s_ui_safe;
}

int moh_timing_enabled(void)
{
    refresh_config();
    return s_timing_enabled && s_gameplay_active;
}

void moh_timing_set_gameplay(CPUState* ctx, int active)
{
    refresh_config();
    active = active ? 1 : 0;
    if (active == s_gameplay_active)
        return;

    if (active)
    {
        s_gameplay_active = 1;
        s_gameplay_ctx = ctx;
        if (ctx->host_call)
            (void)ctx->host_call(ctx, MOH_HOSTCALL_GAMEPLAY_ENTER);

        /* Seed clocks even if FPS unlock is currently off so it can be enabled
         * later from the PC settings menu without reloading the level. */
        s_game_frame_base = mem_read32(ctx, ctx->gpr[13] + (u32)(s32)(-29372));
        s_vsync_seed = mem_read32(ctx, ctx->gpr[13] + (u32)(s32)(-29256));
        s_game_time_seconds = (double)s_game_frame_base / 60.0;
        s_virtual_ticks_elapsed = 0.0;
        s_integer_tick_remainder = 0.0;
        s_integer_ticks = 1;
        s_last_frame_ns = 0;
        s_delta_ticks = 1.0;

        if (s_timing_enabled)
        {
            if (!ctx->host_call || !ctx->host_call(ctx, MOH_HOSTCALL_VI_GAMEPLAY_ON))
            {
                s_timing_enabled = 0;
                fprintf(stderr,
                        "[moh-enh] WARNING: gameplay VI control unavailable; keeping 60-Hz timing\n");
            }
            else
            {
                fprintf(stderr,
                        "[moh-enh] gameplay timing ON: base_frame=%u base_vsync=%u\n",
                        s_game_frame_base, s_vsync_seed);
            }
        }
    }
    else
    {
        if (s_timing_enabled)
        {
            mem_write32(ctx, ctx->gpr[13] + (u32)(s32)(-29372), moh_timing_frame_count());
            if (ctx->host_call)
                (void)ctx->host_call(ctx, MOH_HOSTCALL_VI_GAMEPLAY_OFF);
        }
        if (ctx->host_call)
            (void)ctx->host_call(ctx, MOH_HOSTCALL_GAMEPLAY_EXIT);
        s_gameplay_active = 0;
        s_gameplay_ctx = NULL;
        s_last_frame_ns = 0;
        s_delta_ticks = 1.0;
        s_integer_ticks = 1;
        if (s_timing_enabled)
            fprintf(stderr, "[moh-enh] gameplay timing OFF: restored original VI/menu timing\n");
    }
}

void moh_timing_frame_advance(void)
{
    u64 now;

    refresh_config();
    if (!s_timing_enabled || !s_gameplay_active)
        return;

    now = monotonic_ns();

    if (now && s_last_frame_ns)
    {
        /* Drive simulation from real elapsed host time, not from the requested
         * cap.  If a 144-Hz target only renders at 100 FPS, game speed still
         * remains 1.0x instead of falling to 100/144. */
        u64 elapsed_ns = now > s_last_frame_ns ? now - s_last_frame_ns : 1u;
        if (elapsed_ns < 100000ull)
            elapsed_ns = 100000ull;
        if (elapsed_ns > 100000000ull)
            elapsed_ns = 100000000ull;
        s_delta_ticks = ((double)elapsed_ns / 1000000000.0) * 60.0;
    }
    else if (!s_unlimited && s_target_fps > 0.0)
    {
        /* First gameplay frame only: use the requested cadence until a real
         * elapsed interval exists. */
        s_delta_ticks = 60.0 / s_target_fps;
    }
    else
    {
        s_delta_ticks = 1.0;
    }

    if (now)
        s_last_frame_ns = now;

    if (s_delta_ticks < 0.001)
        s_delta_ticks = 0.001;
    if (s_delta_ticks > 6.0)
        s_delta_ticks = 6.0;

    s_virtual_ticks_elapsed += s_delta_ticks;
    s_game_time_seconds = ((double)s_game_frame_base + s_virtual_ticks_elapsed) / 60.0;

    s_integer_tick_remainder += s_delta_ticks;
    s_integer_ticks = (u32)floor(s_integer_tick_remainder);
    s_integer_tick_remainder -= (double)s_integer_ticks;

    if (!s_timing_logged)
    {
        s_timing_logged = 1;
        if (s_unlimited)
            fprintf(stderr, "[moh-enh] gameplay FPS unlock: uncapped VI, real-time delta\n");
        else
            fprintf(stderr, "[moh-enh] gameplay FPS unlock: %.3f FPS, delta=%.6f ticks\n",
                    s_target_fps, s_delta_ticks);
    }
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
    double ticks = floor(s_virtual_ticks_elapsed);
    if (ticks < 0.0)
        ticks = 0.0;
    if (ticks > 4294967295.0 - (double)s_game_frame_base)
        return 0xffffffffu;
    return s_game_frame_base + (u32)ticks;
}

u32 moh_timing_vsyncs(CPUState* ctx)
{
    (void)ctx;
    refresh_config();
    if (!s_timing_enabled || !s_gameplay_active)
        return 0;

    {
        double ticks = floor(s_virtual_ticks_elapsed);
        if (ticks < 0.0)
            ticks = 0.0;
        if (ticks > 4294967295.0 - (double)s_vsync_seed)
            return 0xffffffffu;
        return s_vsync_seed + (u32)ticks;
    }
}
