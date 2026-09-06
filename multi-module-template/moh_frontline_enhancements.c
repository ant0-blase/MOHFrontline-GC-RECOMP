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

#define MOH_HOSTCALL_SCENE_POSTPROCESS 0xFFFFF150u

#define MOH_HOSTCALL_PS3_FONT_DRAW      0xFFFFF160u
#define MOH_HOSTCALL_PS3_FONT_CENTERED  0xFFFFF161u

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
    if (s_fov_degrees > 104.0)
    {
        static int render_only_fov_cap_logged;
        if (!render_only_fov_cap_logged)
        {
            render_only_fov_cap_logged = 1;
            fprintf(stderr,
                    "[moh-enh] render-only FOV: %.2f requested; "
                    "limiting 4:3 reference FOV to 104.0 (stock CPU culling)\n",
                    s_fov_degrees);
        }
        s_fov_degrees = 104.0;
    }
    if (!(s_weapon_fov_degrees >= 20.0 && s_weapon_fov_degrees < 179.0))
        s_weapon_fov_degrees = -1.0;
    if (!(s_ads_world_fov >= 35.0 && s_ads_world_fov <= 120.0))
        s_ads_world_fov = 72.0;
    if (s_ads_world_fov > 104.0)
        s_ads_world_fov = 104.0;
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

static void moh_reference_fov_tangents(double fov_degrees,
                                       double aspect,
                                       double* out_x,
                                       double* out_y)
{
    /* Keep custom/ADS FOV in the game's native 4:3 reference space, then
     * apply the same Hor+ widening as the normal widescreen path. */
    const double native_tan_x = tan(fov_degrees * (MOH_PI / 360.0));
    const double aspect_scale = aspect / MOH_NATIVE_ASPECT;
    *out_x = native_tan_x * aspect_scale;
    *out_y = native_tan_x / MOH_NATIVE_ASPECT;
}

int moh_wide_fov_force_skybox_faces(void)
{
    static int logged;
    static double cached_ref_fov = -1.0;
    static double cached_aspect = -1.0;
    static double cached_real_hfov = 0.0;

    /*
     * CSkyBox::Draw has its own six-face selector, independent from CFrustum
     * and CCompartment. With Hor+, 105 deg in the PC menu (native 4:3
     * reference) is about 120.16 deg real horizontal at 16:9. The original
     * console selector can then reject a side/top skybox face that is visible.
     *
     * Force all six SKYBOX faces only for a wide real hFOV. This does NOT
     * widen world visibility, PVS, or the CPU frustum.
     */
    if (!s_camera_enabled || !s_gameplay_active || s_fov_degrees <= 0.0)
        return 0;

    if (fabs(cached_ref_fov - s_fov_degrees) > 0.000001 ||
        fabs(cached_aspect - s_aspect) > 0.000001)
    {
        double tan_x;
        double tan_y;

        moh_reference_fov_tangents(s_fov_degrees, s_aspect, &tan_x, &tan_y);
        (void)tan_y;

        cached_ref_fov = s_fov_degrees;
        cached_aspect = s_aspect;
        cached_real_hfov = atan(tan_x) * (360.0 / MOH_PI);
    }

    if (cached_real_hfov < 90.0)
        return 0;

    if (!logged)
    {
        logged = 1;
        fprintf(stderr,
                "[moh-enh] wide-FOV skybox selector bypass: "
                "ref=%.2f real-h=%.2f aspect=%.6f (no CPU frustum overscan)\n",
                s_fov_degrees, cached_real_hfov, s_aspect);
    }

    return 1;
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

        if (ads > 0.000001)
        {
            /* Interpolate in 4:3 reference-FOV space. Aspect correction is
             * applied afterwards, so ADS never squeezes back toward 4:3. */
            const double base_fov =
                s_fov_degrees > 0.0 ? s_fov_degrees :
                atan(tan(original_half_x)) * (360.0 / MOH_PI);
            const double effective_fov =
                base_fov + (s_ads_world_fov - base_fov) * ads;
            moh_reference_fov_tangents(effective_fov, s_aspect,
                                       &tan_half_x, &tan_half_y);
        }
        else if (s_fov_degrees > 0.0)
        {
            moh_reference_fov_tangents(s_fov_degrees, s_aspect,
                                       &tan_half_x, &tan_half_y);
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
                "[moh-enh] aspect-stable camera override: aspect=%.6f fov=%s%.2f\n",
                s_aspect, s_fov_degrees > 0.0 ? "" : "Hor+ ",
                s_fov_degrees > 0.0 ? s_fov_degrees : 0.0);
    }
    return 1;
}

int moh_weapon_projection_override(CPUState* ctx)
{
    double aspect_scale;
    double horizontal_scale;
    double vertical_scale = 1.0;
    double requested_fov;

    refresh_config();
    if (!s_camera_enabled)
        return 0;

    aspect_scale = fabs(s_aspect - MOH_NATIVE_ASPECT) >= 0.000001 ?
        s_aspect / MOH_NATIVE_ASPECT : 1.0;
    horizontal_scale = aspect_scale;

    /* FOV scale and widescreen scale are independent. The previous path
     * replaced Hor+ with the FOV scale, making the weapon look squashed. */
    requested_fov = s_weapon_fov_degrees > 0.0 ? s_weapon_fov_degrees : s_fov_degrees;
    if (requested_fov > 0.0)
    {
        const double original_half = 35.0 * (MOH_PI / 180.0);
        const double requested_half = requested_fov * (MOH_PI / 360.0);
        const double fov_scale = tan(requested_half) / tan(original_half);
        horizontal_scale = fov_scale * aspect_scale;
        vertical_scale = fov_scale;
    }

    {
        const double ads = smooth_ads(moh_ads_blend(ctx));
        if (ads > 0.000001)
        {
            const double original_half = 35.0 * (MOH_PI / 180.0);
            const double ads_half = s_ads_weapon_fov * (MOH_PI / 360.0);
            const double ads_scale = tan(ads_half) / tan(original_half);
            const double ads_horizontal_scale = ads_scale * aspect_scale;
            horizontal_scale += (ads_horizontal_scale - horizontal_scale) * ads;
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
                    "[moh-enh] aspect-stable weapon FOV: %.2f deg (x=%.6f y=%.6f aspect=%.6f)\n",
                    requested_fov, horizontal_scale, vertical_scale, s_aspect);
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

    /*
     * Exact gameplay 3D -> HUD boundary.
     *
     * Do NOT put this in moh_ui_begin(): frontend/menu/VP6 must never receive
     * MOHFrontlineEnhanced.
     */
    if (ctx && ctx->host_call)
        (void)ctx->host_call(ctx, MOH_HOSTCALL_SCENE_POSTPROCESS);

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

static void moh_ps3_font_submit(
    CPUState* ctx,
    u32 token)
{
    u32 saved_r0;
    int replacement_ready;

    if (!ctx ||
        !ctx->host_call)
    {
        return;
    }

    /*
     * Host reads the untouched CFont registers itself and returns success in
     * r0. Preserve r0 because this hook lives inside the original function.
     */
    saved_r0 = ctx->gpr[0];
    ctx->gpr[0] = s_ui_draw_depth ? 4u : 8u;

    (void)ctx->host_call(
        ctx,
        token);

    replacement_ready =
        ctx->gpr[0] == 1u;

    ctx->gpr[0] =
        saved_r0;

    if (replacement_ready)
    {
        /*
         * Host has copied the text/position and will draw the actual PS3 SFNH
         * glyphs in OnScreenUI. Move only this old GC CFont draw outside the
         * viewport, avoiding double text without changing layout/state.
         */
        ctx->fpr[1] = -32768.0;
        ctx->fpr[2] = -32768.0;
    }
}

void moh_hud_text_position_override(CPUState* ctx)
{
    if (!ctx) return;
    if (s_hud_draw_depth) {
        ctx->fpr[1] = 320.0 + (ctx->fpr[1] - 320.0) * s_hud_root_x_scale;
        ctx->fpr[2] = 240.0 + (ctx->fpr[2] - 240.0) * s_hud_root_y_scale;
    } else if (s_ui_draw_depth) {
        ctx->fpr[1] = 320.0 + (ctx->fpr[1] - 320.0) * s_ui_root_x_scale;
        ctx->fpr[2] = 240.0 + (ctx->fpr[2] - 240.0) * s_ui_root_y_scale;
    }
    moh_ps3_font_submit(ctx, MOH_HOSTCALL_PS3_FONT_DRAW);
}

void moh_hud_centered_text_position_override(CPUState* ctx)
{
    if (!ctx) return;
    if (s_hud_draw_depth)
        ctx->fpr[1] = 240.0 + (ctx->fpr[1] - 240.0) * s_hud_root_y_scale;
    else if (s_ui_draw_depth)
        ctx->fpr[1] = 240.0 + (ctx->fpr[1] - 240.0) * s_ui_root_y_scale;
    moh_ps3_font_submit(ctx, MOH_HOSTCALL_PS3_FONT_CENTERED);
}

/* The formatted variant must submit the completed vsprintf buffer, not "%s".
 * 0x8007CD54: r31=CFont, f31=y, stack+132=formatted string. */
void moh_ps3_formatted_font(CPUState* ctx)
{
    u32 r3 = ctx->gpr[3], r4 = ctx->gpr[4];
    f64 f1 = ctx->fpr[1], f2 = ctx->fpr[2];
    ctx->gpr[3] = ctx->gpr[31];
    ctx->gpr[4] = ctx->gpr[1] + 132u;
    ctx->fpr[1] = ctx->fpr[31];
    moh_hud_centered_text_position_override(ctx);
    ctx->fpr[31] = ctx->fpr[1];
    ctx->gpr[3] = r3; ctx->gpr[4] = r4;
    ctx->fpr[1] = f1; ctx->fpr[2] = f2;
}

/* Private metadata hostcalls leave all guest state untouched. */
void moh_ps3_metadata(CPUState* ctx, u32 token, u32 name)
{
    u32 r0;
    if (!ctx || !ctx->host_call) return;
    r0 = ctx->gpr[0];
    ctx->gpr[0] = name;
    (void)ctx->host_call(ctx, token);
    ctx->gpr[0] = r0;
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

/*
 * Frontline intentionally rejects a manual M1 Garand reload while rounds are
 * still present in the en-bloc clip.  The existing generator hooks already run
 * moh_m1_manual_reload_prepare() at 0x800A4A68 (reload input result) and
 * moh_m1_manual_reload_restore() at 0x800A4A88 (after event 17 dispatch).
 *
 * v6h restored the real clip immediately at 0x800A4A88, which is too early for
 * Frontline's BS/script reload logic: the script can process event 17 after the
 * input block has returned, so it saw the original non-empty M1 clip and kept
 * rejecting the reload.
 *
 * Keep the M1 exposed as empty across frames until CWeapon::StartReloading has
 * set its stock "reloading" flag (bit 0x80 at weapon+0x2C0).  The 0x800A4A88
 * hook executes every player update, so it doubles as a cheap poll without any
 * new generated-code hook.  Once the stock reload has really started, restore
 * the exact partial clip; DoneReloading then completes the normal animation and
 * ammo bookkeeping instead of permanently discarding the remaining rounds.
 */
static int s_m1_manual_reload_pending;
static u32 s_m1_manual_reload_weapon;
static u16 s_m1_manual_reload_clip;
static u32 s_m1_manual_reload_wait_frames;
static int s_m1_reload_input_latched;
static int s_m1_manual_reload_logged;
static int s_m1_manual_reload_started_logged;

static int moh_is_cached_mem1(u32 address)
{
    return address >= 0x80000000u && address < 0x81800000u;
}

static u32 moh_player_current_weapon(CPUState* ctx, u32 player)
{
    u32 player_weapon;
    s32 slot;

    if (!ctx || !moh_is_cached_mem1(player))
        return 0u;

    /* Mirror CPlayerObject::GetCurrentWeapon().  Bit 0x40 at +0x396 selects
     * the special weapon pointer at +0x5E0; ordinary inventory weapons use
     * current slot +0x45C and CPlayerWeaponObject::m_weapon at +0x30E4. */
    if ((mem_read8(ctx, player + 0x396u) & 0x40u) != 0u)
        return mem_read32(ctx, player + 0x5E0u);

    slot = (s32)mem_read32(ctx, player + 0x45Cu);
    if (slot < 0 || slot >= 64)
        return 0u;

    player_weapon = mem_read32(ctx, player + 0x460u + (u32)slot * 4u);
    if (!moh_is_cached_mem1(player_weapon))
        return 0u;

    return mem_read32(ctx, player_weapon + 0x30E4u);
}

static void moh_m1_manual_reload_clear_state(void)
{
    s_m1_manual_reload_pending = 0;
    s_m1_manual_reload_weapon = 0u;
    s_m1_manual_reload_clip = 0u;
    s_m1_manual_reload_wait_frames = 0u;
}

void moh_m1_manual_reload_restore(CPUState* ctx)
{
    u32 weapon;
    u32 current_weapon;
    u8 flags;
    int reload_started;

    if (!ctx || !s_m1_manual_reload_pending)
        return;

    weapon = s_m1_manual_reload_weapon;
    if (!moh_is_cached_mem1(weapon))
    {
        moh_m1_manual_reload_clear_state();
        return;
    }

    /*
     * A fast weapon switch used to leave the old M1 fake-empty until the
     * timeout expired. Restore it immediately as soon as the player's current
     * weapon changes. r31 is still the live CPlayerObject* at the 0x800A4A88
     * poll site.
     */
    current_weapon = moh_player_current_weapon(ctx, ctx->gpr[31]);
    if (moh_is_cached_mem1(current_weapon) && current_weapon != weapon)
    {
        if (mem_read16(ctx, weapon + 0x286u) == 0u)
            mem_write16(ctx, weapon + 0x286u, s_m1_manual_reload_clip);

        fprintf(stderr,
                "[moh-enh] M1 tactical reload cancelled by weapon switch; "
                "restored clip=%u\\n",
                (unsigned)s_m1_manual_reload_clip);
        moh_m1_manual_reload_clear_state();
        return;
    }

    /* CWeapon::StartReloading @ 0x800D61C4 sets bit 0x80 in +0x2C0.
     * Do NOT restore the partial clip until that real stock state is reached. */
    flags = mem_read8(ctx, weapon + 0x2C0u);
    reload_started = (flags & 0x80u) != 0u;

    if (!reload_started)
    {
        ++s_m1_manual_reload_wait_frames;

        /* A swallowed/cancelled event must not make the M1 unable to fire for
         * several seconds. One second at 60 Hz is ample for the stock script
         * to enter StartReloading. */
        if (s_m1_manual_reload_wait_frames < 60u)
            return;
    }

    if (mem_read16(ctx, weapon + 0x286u) == 0u)
        mem_write16(ctx, weapon + 0x286u, s_m1_manual_reload_clip);

    if (reload_started && !s_m1_manual_reload_started_logged)
    {
        s_m1_manual_reload_started_logged = 1;
        fprintf(stderr,
                "[moh-enh] M1 Garand tactical reload accepted by stock reload state "
                "(restored clip=%u after %u frame(s))\\n",
                (unsigned)s_m1_manual_reload_clip,
                (unsigned)s_m1_manual_reload_wait_frames);
    }
    else if (!reload_started)
    {
        fprintf(stderr,
                "[moh-enh] WARNING: M1 tactical reload cancelled/timed out; "
                "restoring clip\\n");
    }

    moh_m1_manual_reload_clear_state();
}

void moh_m1_manual_reload_prepare(CPUState* ctx)
{
    u32 player;
    u32 weapon;
    u32 properties;
    u32 weapon_type;
    s16 clip;
    s16 reserve;
    s16 max_clip;

    if (!ctx)
        return;

    /* Garand tactical reload is edge-triggered. Holding/spamming R previously
     * re-armed event 17 immediately after StartReloading and could wedge fire. */
    if (ctx->gpr[3] == 0u)
    {
        s_m1_reload_input_latched = 0;
        return;
    }

    player = ctx->gpr[31];
    weapon = moh_player_current_weapon(ctx, player);
    if (!moh_is_cached_mem1(weapon))
        return;

    weapon_type = mem_read32(ctx, weapon + 0x298u);
    properties = mem_read32(ctx, weapon + 0x280u);
    if (!moh_is_cached_mem1(properties))
        return;
    max_clip = (s16)mem_read16(ctx, properties + 0x04u);

    if ((weapon_type != 14u && weapon_type != 20u) || max_clip != 8)
        return;

    if (s_m1_reload_input_latched)
    {
        ctx->gpr[3] = 0u;
        return;
    }
    s_m1_reload_input_latched = 1;

    if (s_m1_manual_reload_pending)
    {
        ctx->gpr[3] = 0u;
        return;
    }

    reserve = (s16)mem_read16(ctx, weapon + 0x284u);
    clip = (s16)mem_read16(ctx, weapon + 0x286u);

    /* Full/no-reserve reload is ignored completely. Empty clip keeps stock
     * behavior. Only a genuine partial Garand clip enters the compatibility path. */
    if (clip >= max_clip || reserve <= 0)
    {
        ctx->gpr[3] = 0u;
        return;
    }
    if (clip <= 0)
        return;

    s_m1_manual_reload_pending = 1;
    s_m1_manual_reload_weapon = weapon;
    s_m1_manual_reload_clip = (u16)clip;
    s_m1_manual_reload_wait_frames = 0u;
    mem_write16(ctx, weapon + 0x286u, 0u);

    if (!s_m1_manual_reload_logged)
    {
        s_m1_manual_reload_logged = 1;
        fprintf(stderr,
                "[moh-enh] M1 Garand partial-clip tactical reload armed "
                "(type=%u clip=%d/%d reserve=%d)\n",
                weapon_type, (int)clip, (int)max_clip, (int)reserve);
    }
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

    /* This existing per-frame player hook also polls the M1 pending state,
     * so weapon switches/cancels restore the temporary clip even without R. */
    moh_m1_manual_reload_restore(ctx);

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
