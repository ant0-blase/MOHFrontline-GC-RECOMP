#define _POSIX_C_SOURCE 200809L

#include "moh_frontline_enhancements.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define MOH_NATIVE_ASPECT (4.0 / 3.0)
#define MOH_PI 3.14159265358979323846264338327950288

static int s_camera_enabled;
static int s_timing_enabled;
static int s_ui_safe;
static double s_hud_scale = 1.0;
static double s_hud_safe_width = 1.0;
static int s_gameplay_active;
static int s_unlimited;
static double s_aspect = MOH_NATIVE_ASPECT;
static double s_fov_degrees;
static double s_weapon_fov_degrees = -1.0;
static int s_ads_enabled;
static double s_ads_world_fov = 72.0;
static double s_ads_weapon_fov = 68.0;
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
static double s_ui_applied_hud_scale = -1.0;
static double s_ui_applied_safe_width = -1.0;
static int s_ui_applied_safe = -1;
static int s_ui_logged;

/* Frontend UIS root state. MOHF's IStudio/UIS coordinates are native
 * 640x480 pixels.  The old v9.1 hook incorrectly used 2048,2048 as the
 * centre; 2048 belongs to the low-level sprite/font projection offsets and
 * pushed frontend screens outside the visible viewport. */
static unsigned s_ui_draw_depth;
static u32 s_ui_saved_matrix[16];
static u32 s_ui_matrix_address;
static double s_ui_root_x_scale = 1.0;
static double s_ui_root_y_scale = 1.0;
static int s_ui_root_logged;

/* The in-game HUD does not use IStudio. UserInterface::Draw builds 640x480
 * spritepolyvert arrays directly and draws CFont text separately. Scope the
 * HUD draw and patch only those transient vertices/text coordinates. */
#define MOH_HUD_MAX_POLY_VERTS 128u
static unsigned s_hud_draw_depth;
static u32 s_hud_poly_address;
static u32 s_hud_poly_count;
static u32 s_hud_saved_x[MOH_HUD_MAX_POLY_VERTS];
static u32 s_hud_saved_y[MOH_HUD_MAX_POLY_VERTS];
static double s_hud_root_x_scale = 1.0;
static double s_hud_root_y_scale = 1.0;
static int s_hud_logged;

#define MOH_HOSTCALL_VI_GAMEPLAY_ON  0xFFFFF100u
#define MOH_HOSTCALL_VI_GAMEPLAY_OFF 0xFFFFF101u
#define MOH_HOSTCALL_GAMEPLAY_ENTER   0xFFFFF110u
#define MOH_HOSTCALL_GAMEPLAY_EXIT    0xFFFFF111u
#define MOH_HOSTCALL_ADS_STATE        0xFFFFF131u
#define MOH_HOSTCALL_FRAME_PRESENT     0xFFFFF133u

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
    s_hud_scale = env_double("MOH_HUD_SCALE", 1.0);
    s_hud_safe_width = env_double("MOH_HUD_SAFE_WIDTH", 1.0);
    s_aspect = env_double("MOH_ASPECT_VALUE", MOH_NATIVE_ASPECT);
    s_fov_degrees = env_double("MOH_FOV_DEGREES", 0.0);
    s_weapon_fov_degrees = env_double("MOH_WEAPON_FOV_DEGREES", -1.0);
    s_ads_enabled = env_enabled("MOH_FPS_ADS");
    s_ads_world_fov = env_double("MOH_ADS_WORLD_FOV", 72.0);
    s_ads_weapon_fov = env_double("MOH_ADS_WEAPON_FOV", 68.0);

    if (!(s_aspect > 0.5 && s_aspect < 10.0))
        s_aspect = MOH_NATIVE_ASPECT;
    if (!(s_hud_scale >= 0.50 && s_hud_scale <= 1.50))
        s_hud_scale = 1.0;
    if (!(s_hud_safe_width >= 0.70 && s_hud_safe_width <= 1.00))
        s_hud_safe_width = 1.0;
    if (!(s_fov_degrees >= 0.0 && s_fov_degrees < 179.0))
        s_fov_degrees = 0.0;
    if (!(s_weapon_fov_degrees >= 20.0 && s_weapon_fov_degrees < 179.0))
        s_weapon_fov_degrees = -1.0;
    if (!(s_ads_world_fov >= 35.0 && s_ads_world_fov <= 120.0))
        s_ads_world_fov = 72.0;
    if (!(s_ads_weapon_fov >= 35.0 && s_ads_weapon_fov <= 120.0))
        s_ads_weapon_fov = 68.0;

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
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    u64 whole;
    u64 remainder;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0 || counter.QuadPart < 0)
        return 0;
    whole = (u64)counter.QuadPart / (u64)frequency.QuadPart;
    remainder = (u64)counter.QuadPart % (u64)frequency.QuadPart;
    return whole * 1000000000ull +
           (remainder * 1000000000ull) / (u64)frequency.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
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

static double moh_ads_blend(CPUState* ctx)
{
    u32 saved_r0;
    double blend;
    if (!s_ads_enabled || !ctx || !ctx->host_call)
        return 0.0;
    saved_r0 = ctx->gpr[0];
    if (!ctx->host_call(ctx, MOH_HOSTCALL_ADS_STATE))
    {
        ctx->gpr[0] = saved_r0;
        return 0.0;
    }
    blend = (double)f32_from_bits(ctx->gpr[0]);
    ctx->gpr[0] = saved_r0;
    if (!isfinite(blend))
        return 0.0;
    return fmin(fmax(blend, 0.0), 1.0);
}

static double smooth_ads(double x)
{
    x = fmin(fmax(x, 0.0), 1.0);
    return x * x * (3.0 - 2.0 * x);
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

    {
        const double ads = smooth_ads(moh_ads_blend(ctx));
        double base_fov;
        if (s_fov_degrees > 0.0)
        {
            base_fov = s_fov_degrees;
        }
        else
        {
            double base_tan = tan(original_half_x);
            if (fabs(s_aspect - MOH_NATIVE_ASPECT) >= 0.000001)
                base_tan *= s_aspect / MOH_NATIVE_ASPECT;
            base_fov = atan(base_tan) * (360.0 / MOH_PI);
        }

        if (ads > 0.000001)
        {
            const double effective_fov = base_fov + (s_ads_world_fov - base_fov) * ads;
            tan_half_x = tan(effective_fov * (MOH_PI / 360.0));
            tan_half_y = tan_half_x / s_aspect;
        }
        else if (s_fov_degrees > 0.0)
        {
            tan_half_x = tan(s_fov_degrees * (MOH_PI / 360.0));
            tan_half_y = tan_half_x / s_aspect;
        }
        else if (fabs(s_aspect - MOH_NATIVE_ASPECT) < 0.000001)
        {
            tan_half_x = tan(original_half_x);
            tan_half_y = tan(original_half_y);
        }
        else
        {
            tan_half_x = tan(original_half_x) * (s_aspect / MOH_NATIVE_ASPECT);
            tan_half_y = tan(original_half_y);
        }
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

    {
        const double ads = smooth_ads(moh_ads_blend(ctx));
        if (ads > 0.000001)
        {
            const double original_half = 35.0 * (MOH_PI / 180.0);
            const double ads_half = s_ads_weapon_fov * (MOH_PI / 360.0);
            const double ads_scale = tan(ads_half) / tan(original_half);
            horizontal_scale += (ads_scale - horizontal_scale) * ads;
            vertical_scale += (ads_scale - vertical_scale) * ads;
        }
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
    if (s_ui_applied_safe != s_ui_safe ||
        fabs(s_ui_applied_aspect - s_aspect) > 0.000001 ||
        fabs(s_ui_applied_hud_scale - s_hud_scale) > 0.000001 ||
        fabs(s_ui_applied_safe_width - s_hud_safe_width) > 0.000001)
        mem_write8(ctx, screen + 160u, 0u);
}

void moh_ui_matrix_override(CPUState* ctx)
{
    const u32 screen = ctx->gpr[31];
    const u32 matrix = screen + 96u;
    double x_scale;
    double y_scale;
    unsigned x_offsets[3] = {0u, 4u, 8u};
    unsigned y_offsets[3] = {16u, 20u, 24u};
    int i;

    refresh_config();
    x_scale = s_hud_scale * s_hud_safe_width;
    y_scale = s_hud_scale;

    if (s_ui_safe && fabs(s_aspect - MOH_NATIVE_ASPECT) > 0.000001)
    {
        if (s_aspect > MOH_NATIVE_ASPECT)
            x_scale *= MOH_NATIVE_ASPECT / s_aspect;
        else
            y_scale *= s_aspect / MOH_NATIVE_ASPECT;
    }

    if (fabs(x_scale - 1.0) > 0.000001)
    {
        for (i = 0; i < 3; ++i)
        {
            const f32 old_value = f32_from_bits(mem_read32(ctx, matrix + x_offsets[i]));
            mem_write32(ctx, matrix + x_offsets[i], f32_bits((f32)(old_value * x_scale)));
        }
    }
    if (fabs(y_scale - 1.0) > 0.000001)
    {
        for (i = 0; i < 3; ++i)
        {
            const f32 old_value = f32_from_bits(mem_read32(ctx, matrix + y_offsets[i]));
            mem_write32(ctx, matrix + y_offsets[i], f32_bits((f32)(old_value * y_scale)));
        }
    }

    if (!s_ui_logged && (fabs(x_scale - 1.0) > 0.000001 || fabs(y_scale - 1.0) > 0.000001))
    {
        s_ui_logged = 1;
        fprintf(stderr,
                "[moh-enh] aspect-correct 2D UI active: aspect=%.6f x-scale=%.6f y-scale=%.6f\n",
                s_aspect, x_scale, y_scale);
    }

    s_ui_applied_aspect = s_aspect;
    s_ui_applied_safe = s_ui_safe;
    s_ui_applied_hud_scale = s_hud_scale;
    s_ui_applied_safe_width = s_hud_safe_width;
}

static int moh_frontend_compute_scales(double* out_x, double* out_y)
{
    double x_scale = 1.0;
    double y_scale = 1.0;

    refresh_config();

    /* Frontend screens were authored for 640x480. Preserve their original
     * size and safe 4:3 composition; HUD Scale/Safe Width are gameplay HUD
     * controls and must not shrink the frontend itself. */
    if (s_ui_safe && fabs(s_aspect - MOH_NATIVE_ASPECT) > 0.000001)
    {
        if (s_aspect > MOH_NATIVE_ASPECT)
            x_scale = MOH_NATIVE_ASPECT / s_aspect;
        else
            y_scale = s_aspect / MOH_NATIVE_ASPECT;
    }

    if (!isfinite(x_scale) || !isfinite(y_scale) || x_scale <= 0.0 || y_scale <= 0.0)
        return 0;
    *out_x = x_scale;
    *out_y = y_scale;
    return 1;
}

static int moh_hud_compute_scales(double* out_x, double* out_y)
{
    double x_scale;
    double y_scale;

    refresh_config();
    x_scale = s_hud_scale * s_hud_safe_width;
    y_scale = s_hud_scale;

    if (s_ui_safe && fabs(s_aspect - MOH_NATIVE_ASPECT) > 0.000001)
    {
        if (s_aspect > MOH_NATIVE_ASPECT)
            x_scale *= MOH_NATIVE_ASPECT / s_aspect;
        else
            y_scale *= s_aspect / MOH_NATIVE_ASPECT;
    }

    if (!isfinite(x_scale) || !isfinite(y_scale) || x_scale <= 0.0 || y_scale <= 0.0)
        return 0;
    *out_x = x_scale;
    *out_y = y_scale;
    return 1;
}

void moh_ui_begin(CPUState* ctx)
{
    const u32 g_mat_stack_addr = 0x8034EAB8u;
    const f32 logical_center_x = 320.0f;
    const f32 logical_center_y = 240.0f;
    u32 stack;
    u32 index;
    u32 base;
    u32 matrix;
    double x_scale_d;
    double y_scale_d;
    f32 x_scale;
    f32 y_scale;
    int i;

    if (!ctx)
        return;

    if (s_ui_draw_depth++ != 0u)
        return;

    if (!moh_frontend_compute_scales(&x_scale_d, &y_scale_d))
        goto fail;

    stack = mem_read32(ctx, g_mat_stack_addr);
    if (stack < 0x80000000u || stack >= 0x81800000u)
        goto fail;
    index = mem_read32(ctx, stack + 4u);
    base = mem_read32(ctx, stack + 8u);
    if (base < 0x80000000u || base >= 0x81800000u || index >= 64u)
        goto fail;
    matrix = base + index * 64u;
    if (matrix < 0x80000000u || matrix > 0x817FFFC0u)
        goto fail;

    for (i = 0; i < 16; ++i)
        s_ui_saved_matrix[i] = mem_read32(ctx, matrix + (u32)i * 4u);
    s_ui_matrix_address = matrix;

    x_scale = (f32)x_scale_d;
    y_scale = (f32)y_scale_d;
    s_ui_root_x_scale = x_scale_d;
    s_ui_root_y_scale = y_scale_d;

    /* UIS vertices/text anchors are authored directly in 640x480 space.
     * Scale around the real native centre, not the low-level 2048 projection
     * offset used by CSprite/CFont internals:
     *   p' = centre + scale * (p - centre) */
    for (i = 0; i < 3; ++i)
    {
        const u32 component = (u32)i * 4u;
        const f32 right = f32_from_bits(s_ui_saved_matrix[component / 4u]);
        const f32 front = f32_from_bits(s_ui_saved_matrix[(16u + component) / 4u]);
        const f32 pos = f32_from_bits(s_ui_saved_matrix[(48u + component) / 4u]);
        const f32 centred = pos +
            right * logical_center_x * (1.0f - x_scale) +
            front * logical_center_y * (1.0f - y_scale);

        mem_write32(ctx, matrix + component, f32_bits(right * x_scale));
        mem_write32(ctx, matrix + 16u + component, f32_bits(front * y_scale));
        mem_write32(ctx, matrix + 48u + component, f32_bits(centred));
    }

    if (!s_ui_root_logged &&
        (fabs(x_scale_d - 1.0) > 0.000001 || fabs(y_scale_d - 1.0) > 0.000001))
    {
        s_ui_root_logged = 1;
        fprintf(stderr,
                "[moh-enh] frontend UIS aspect fix active: native=640x480 aspect=%.6f x=%.6f y=%.6f\n",
                s_aspect, x_scale_d, y_scale_d);
    }
    return;

fail:
    s_ui_draw_depth = 0u;
    s_ui_matrix_address = 0u;
    s_ui_root_x_scale = 1.0;
    s_ui_root_y_scale = 1.0;
}

void moh_ui_end(CPUState* ctx)
{
    int i;

    if (!ctx || s_ui_draw_depth == 0u)
        return;
    if (--s_ui_draw_depth != 0u)
        return;

    if (s_ui_matrix_address >= 0x80000000u && s_ui_matrix_address <= 0x817FFFC0u)
    {
        for (i = 0; i < 16; ++i)
            mem_write32(ctx, s_ui_matrix_address + (u32)i * 4u, s_ui_saved_matrix[i]);
    }
    s_ui_matrix_address = 0u;
    s_ui_root_x_scale = 1.0;
    s_ui_root_y_scale = 1.0;
}

static int s_hud_hide_guest_crosshair = 0;
static int s_hud_crosshair_log_once = 0;

static int moh_query_hide_guest_crosshair(CPUState* ctx)
{
    u32 saved_r3;
    int hide;

    if (!ctx || !ctx->host_call)
        return 0;

    /*
     * 0xFFFFF132 = native crosshair host call.
     * Use r3=1 as sentinel. Host changes r3 to 0 when the original
     * GMFE69 crosshair must be suppressed.
     */
    saved_r3 = ctx->gpr[3];
    ctx->gpr[3] = 1u;

    (void)ctx->host_call(ctx, 0xFFFFF132u);

    hide = ctx->gpr[3] == 0u;
    ctx->gpr[3] = saved_r3;

    return hide;
}

void moh_hud_begin(CPUState* ctx)
{
    double x_scale;
    double y_scale;

    if (s_hud_draw_depth++ != 0u)
        return;

    s_hud_hide_guest_crosshair = moh_query_hide_guest_crosshair(ctx);

    if (!moh_hud_compute_scales(&x_scale, &y_scale))
    {
        s_hud_draw_depth = 0u;
        s_hud_root_x_scale = 1.0;
        s_hud_root_y_scale = 1.0;
        return;
    }

    s_hud_root_x_scale = x_scale;
    s_hud_root_y_scale = y_scale;

    if (!s_hud_logged &&
        (fabs(x_scale - 1.0) > 0.000001 || fabs(y_scale - 1.0) > 0.000001))
    {
        s_hud_logged = 1;
        fprintf(stderr,
                "[moh-enh] gameplay HUD transform active: native=640x480 aspect=%.6f x=%.6f y=%.6f\n",
                s_aspect, x_scale, y_scale);
    }
}

void moh_hud_end(CPUState* ctx)
{
    (void)ctx;
    if (s_hud_draw_depth == 0u)
        return;
    if (--s_hud_draw_depth != 0u)
        return;

    s_hud_root_x_scale = 1.0;
    s_hud_root_y_scale = 1.0;
    s_hud_hide_guest_crosshair = 0;
}

void moh_hud_poly_begin(CPUState* ctx)
{
    const f32 center_x = 320.0f;
    const f32 center_y = 240.0f;
    const u32 verts = ctx ? ctx->gpr[4] : 0u;
    const u32 count = ctx ? ctx->gpr[5] : 0u;

    f32 min_x = 1000000.0f;
    f32 max_x = -1000000.0f;
    f32 min_y = 1000000.0f;
    f32 max_y = -1000000.0f;

    int kill_guest_crosshair = 0;
    u32 i;

    s_hud_poly_address = 0u;
    s_hud_poly_count = 0u;

    if (!ctx || s_hud_draw_depth == 0u ||
        count == 0u || count > MOH_HUD_MAX_POLY_VERTS)
        return;

    if (verts < 0x80000000u ||
        verts > 0x817FFFFFu - count * 24u)
        return;

    /* Inspect original 640x480 HUD coordinates first. */
    for (i = 0; i < count; ++i)
    {
        const u32 vertex = verts + i * 24u;
        const f32 x = f32_from_bits(mem_read32(ctx, vertex + 8u));
        const f32 y = f32_from_bits(mem_read32(ctx, vertex + 12u));

        if (!isfinite(x) || !isfinite(y))
            return;

        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }

    /*
     * Original MOH reticle is small geometry centred around 320x240.
     * Only run this detector when the native PC crosshair explicitly
     * requests replacement of the guest one.
     */
    if (s_hud_hide_guest_crosshair && count == 4u)
    {
        const f32 width = max_x - min_x;
        const f32 height = max_y - min_y;

        const int near_center =
            max_x >= center_x - 36.0f &&
            min_x <= center_x + 36.0f &&
            max_y >= center_y - 36.0f &&
            min_y <= center_y + 36.0f;

        const int reticle_size =
            width >= 1.0f && width <= 96.0f &&
            height >= 1.0f && height <= 96.0f;

        if (near_center && reticle_size)
            kill_guest_crosshair = 1;
    }

    for (i = 0; i < count; ++i)
    {
        const u32 vertex = verts + i * 24u;
        const u32 x_bits = mem_read32(ctx, vertex + 8u);
        const u32 y_bits = mem_read32(ctx, vertex + 12u);

        const f32 x = f32_from_bits(x_bits);
        const f32 y = f32_from_bits(y_bits);

        s_hud_saved_x[i] = x_bits;
        s_hud_saved_y[i] = y_bits;

        if (kill_guest_crosshair)
        {
            /* Remove only transient draw geometry. */
            mem_write32(ctx, vertex + 8u, f32_bits(-4096.0f));
            mem_write32(ctx, vertex + 12u, f32_bits(-4096.0f));
        }
        else
        {
            const f32 out_x =
                center_x + (x - center_x) * (f32)s_hud_root_x_scale;
            const f32 out_y =
                center_y + (y - center_y) * (f32)s_hud_root_y_scale;

            mem_write32(ctx, vertex + 8u, f32_bits(out_x));
            mem_write32(ctx, vertex + 12u, f32_bits(out_y));
        }
    }

    if (kill_guest_crosshair && !s_hud_crosshair_log_once)
    {
        s_hud_crosshair_log_once = 1;

        fprintf(stderr,
                "[moh-enh] original crosshair draw suppressed "
                "(%.1f,%.1f -> %.1f,%.1f)\n",
                (double)min_x,
                (double)min_y,
                (double)max_x,
                (double)max_y);
    }

    s_hud_poly_address = verts;
    s_hud_poly_count = count;
}

void moh_hud_poly_end(CPUState* ctx)
{
    u32 i;

    if (!ctx || s_hud_poly_address == 0u || s_hud_poly_count == 0u)
        return;

    for (i = 0; i < s_hud_poly_count; ++i)
    {
        const u32 vertex = s_hud_poly_address + i * 24u;
        mem_write32(ctx, vertex + 8u, s_hud_saved_x[i]);
        mem_write32(ctx, vertex + 12u, s_hud_saved_y[i]);
    }
    s_hud_poly_address = 0u;
    s_hud_poly_count = 0u;
}

void moh_hud_text_position_override(CPUState* ctx)
{
    const double center_x = 320.0;
    const double center_y = 240.0;

    if (!ctx || s_hud_draw_depth == 0u)
        return;

    ctx->fpr[1] = center_x + (ctx->fpr[1] - center_x) * s_hud_root_x_scale;
    ctx->fpr[2] = center_y + (ctx->fpr[2] - center_y) * s_hud_root_y_scale;
}

void moh_hud_centered_text_position_override(CPUState* ctx)
{
    const double center_y = 240.0;

    if (!ctx || s_hud_draw_depth == 0u)
        return;

    /* DrawTextCentered computes horizontal centring internally; its only float
     * argument is the vertical position. */
    ctx->fpr[1] = center_y + (ctx->fpr[1] - center_y) * s_hud_root_y_scale;
}

void moh_ui_font_scale_override(CPUState* ctx)
{
    double x_scale = 1.0;
    double y_scale = 1.0;

    if (!ctx)
        return;

    if (s_hud_draw_depth != 0u)
    {
        x_scale = s_hud_root_x_scale;
        y_scale = s_hud_root_y_scale;
    }
    else if (s_ui_draw_depth != 0u)
    {
        x_scale = s_ui_root_x_scale;
        y_scale = s_ui_root_y_scale;
    }
    else
    {
        return;
    }

    ctx->fpr[1] *= x_scale;
    ctx->fpr[2] *= y_scale;
}

int moh_timing_enabled(void)
{
    refresh_config();
    return s_timing_enabled && s_gameplay_active;
}

void moh_player_vertical_delta_fix(CPUState* ctx)
{
    double delta;

    if (!ctx)
        return;
    refresh_config();
    if (!s_timing_enabled || !s_gameplay_active)
        return;

    delta = ctx->fpr[28];
    if (!isfinite(delta) || delta <= 0.0 || delta >= 1.0)
        return;

    /* Original instruction at 0x800A310C has just computed f0 = vertical/delta.
     * For sub-1 deltas this artificially amplifies the contact/step correction.
     * Multiplying by the same delta restores the original vertical value while
     * leaving the rest of BeginUpdate's fractional-delta logic untouched. */
    ctx->fpr[0] *= delta * delta;
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


void moh_timing_arm_present(CPUState* ctx)
{
    refresh_config();
    if (!ctx || !s_timing_enabled || !s_gameplay_active || !ctx->host_call)
        return;
    (void)ctx->host_call(ctx, MOH_HOSTCALL_FRAME_PRESENT);
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
