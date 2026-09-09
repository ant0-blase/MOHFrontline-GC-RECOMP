#!/usr/bin/env python3
# Native-PC hot-function postgen for Medal of Honor: Frontline GMFE69.
# Logs distinguish real host-native execution from recomp PPC fallback.
# No GX FIFO write batching or reordering is performed.

from __future__ import annotations

import re
from pathlib import Path

MARK = "MOH_GMFE69_PC_NATIVE_V1"
HELPER_MARK = "MOH_GMFE69_PC_NATIVE_HELPERS"

FALLBACKS = {
    0x80075024: "CCompartment::DrawNode(CPTNode&,lights)",
    0x8012B08C: "GXSetVtxDesc",
    0x8012B5A0: "GXSetVtxAttrFmt",
    0x8012C3C8: "GXBegin",
    0x8012DA0C: "GXInitTexObj",
    0x8012DCC8: "GXInitTexObjLOD",
    0x8012E02C: "GXLoadTexObj",
}

PARTIALS = {
    0x8007D704:
        "CParticleSystem::RenderSystem | native math/stack/WPAR; GX/body recomp",
}

NATIVE_VOID_NOOPS = {
    0x800F3578: "PROFILE_UPDATE",
    0x800F4120: "DebugProfTimerStop",
    0x800F41A0: "DebugProfTimerResume",
    0x800F41F0: "DebugProfTimerPause",
    0x800F4240: "DebugProfTimerStart",
}

NATIVE_UNARY_DOUBLE = {
    0x80143228: ("cos", "cos"),
    0x801432FC: ("floor", "floor"),
    0x80143790: ("sin", "sin"),
    0x80143868: ("tan", "tan"),
    0x801438E0: ("acos", "acos"),
    0x80143900: ("asin", "asin"),
}
NATIVE_BINARY_DOUBLE = {
    0x80143920: ("atan2", "atan2"),
    0x80143940: ("fmod", "fmod"),
    0x80143960: ("pow", "pow"),
}
NATIVE_UNARY_FLOAT = {
    0x80143980: ("cosf", "cosf"),
}
LDEXP_PC = 0x801434D0

GAMELOOP_DELTA_PC = 0x80018D28
GET_GAME_TIME_PC = 0x80017E2C
GET_FRAME_COUNT_PC = 0x80017E60

HELPERS = r'''
/* MOH_GMFE69_PC_NATIVE_HELPERS */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static int moh_pc_native_timing_enabled(void)
{
    static int init = 0;
    static int enabled = 1;
    if (!init) {
        const char* v = getenv("MOH_PC_NATIVE_TIMING");
        enabled = !(v && (*v == '0' || *v == 'n' || *v == 'N' ||
                          *v == 'f' || *v == 'F'));
        init = 1;
    }
    return enabled;
}

static int moh_pc_native_debug_prof_enabled(void)
{
    static int init = 0;
    static int enabled = 1;
    if (!init) {
        const char* v = getenv("MOH_PC_NATIVE_DEBUG_PROF");
        enabled = !(v && (*v == '0' || *v == 'n' || *v == 'N' ||
                          *v == 'f' || *v == 'F'));
        init = 1;
    }
    return enabled;
}

static double moh_pc_now_seconds(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int ready = 0;
    LARGE_INTEGER now;
    if (!ready) {
        QueryPerformanceFrequency(&freq);
        ready = 1;
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
#endif
}

typedef struct MohPcNativeTiming {
    int initialized;
    double start;
    double previous;
    double time;
    double delta;
    double fps;
    double last_log;
    uint32_t frame;
} MohPcNativeTiming;

static MohPcNativeTiming g_moh_pc_timing;

static double moh_pc_native_frame_tick(void)
{
    const double now = moh_pc_now_seconds();
    MohPcNativeTiming* t = &g_moh_pc_timing;

    if (!t->initialized) {
        t->initialized = 1;
        t->start = now;
        t->previous = now;
        t->last_log = now;
        t->delta = 1.0 / 60.0;
        t->fps = 60.0;
        t->frame = 0;
        fprintf(stderr,
                "[MOH-PC] PC Natif | FPS/Timing/Delta metrics | host monotonic | gameplay clock untouched\n");
    } else {
        double dt = now - t->previous;
        t->previous = now;

        if (!(dt > 0.0))
            dt = 1.0 / 60.0;
        if (dt < 1.0 / 1000.0)
            dt = 1.0 / 1000.0;
        if (dt > 0.100)
            dt = 0.100;

        t->delta = dt;
        {
            const double inst = 1.0 / dt;
            t->fps = t->fps * 0.90 + inst * 0.10;
        }
    }

    t->time = now - t->start;
    ++t->frame;

    if (now - t->last_log >= 1.0) {
        fprintf(stderr,
                "[MOH-PC] PC Natif | FPS %.2f | Timing %.3fs | Delta %.3f ms | Frame %u\n",
                t->fps, t->time, t->delta * 1000.0, t->frame);
        t->last_log = now;
    }

    return t->delta;
}

static void moh_pc_log_native_once(const char* name, u32 pc, int* once)
{
    if (!*once) {
        fprintf(stderr, "[MOH-PC] PC Natif | %08X | %s\n", pc, name);
        *once = 1;
    }
}

static void moh_pc_log_fallback_once(const char* name, u32 pc, int* once)
{
    if (!*once) {
        fprintf(stderr, "[MOH-PC] Fallback | %08X | %s\n", pc, name);
        *once = 1;
    }
}

static void moh_pc_log_partial_once(const char* name, u32 pc, int* once)
{
    if (!*once) {
        fprintf(stderr, "[MOH-PC] PC Natif partiel | %08X | %s\n", pc, name);
        *once = 1;
    }
}
'''

def _find_chunk_for_pc(chunks: Path, pc: int) -> Path | None:
    label = f"label_{pc:08X}:"
    hits = []
    for path in sorted(chunks.glob("*.c")):
        try:
            text = path.read_text(errors="ignore")
        except OSError:
            continue
        if label in text:
            hits.append(path)
    if len(hits) > 1:
        raise RuntimeError(f"{pc:08X}: label found in multiple chunks: {hits}")
    return hits[0] if hits else None

def _ensure_helpers(path: Path) -> str:
    text = path.read_text()
    if HELPER_MARK in text:
        return text
    anchor = '#include "../generated.h"\n'
    if anchor not in text:
        raise RuntimeError(f"{path}: generated.h include anchor missing")
    return text.replace(anchor, anchor + "\n" + HELPERS + "\n", 1)

def _inject_after_label(path: Path, pc: int, body: str, marker: str) -> bool:
    text = _ensure_helpers(path)
    label = f"label_{pc:08X}:\n"
    if marker in text:
        return False
    if text.count(label) != 1:
        raise RuntimeError(f"{path}: expected one {label.strip()}")
    text = text.replace(label, label + body, 1)
    path.write_text(text)
    return True

def _native_prefix(pc: int, name: str) -> str:
    return (
        f"    /* {MARK}: native {name} */\n"
        f"    static int moh_native_once_{pc:08X};\n"
        f'    moh_pc_log_native_once("{name}", 0x{pc:08X}u, '
        f"&moh_native_once_{pc:08X});\n"
    )

def _patch_timing(chunks: Path) -> int:
    """Host-native timing metrics only; gameplay timing stays original."""
    count = 0

    # 0x80018D20 has already loaded Frontline's original timestep into f31.
    # Sample host time at 0x80018D28 but never overwrite guest timing state.
    p = _find_chunk_for_pc(chunks, GAMELOOP_DELTA_PC)
    if p:
        body = f'''    /* {MARK}: native PC timing monitor only */
    if (moh_pc_native_timing_enabled()) {{
        (void)moh_pc_native_frame_tick();
    }}
'''
        count += _inject_after_label(
            p, GAMELOOP_DELTA_PC, body,
            f"{MARK}: native PC timing monitor only"
        )

    # Keep game clock/counter semantics untouched; log honest fallbacks.
    for pc, name in (
        (GET_GAME_TIME_PC, "GetGameTime (gameplay clock preserved)"),
        (GET_FRAME_COUNT_PC, "GetFrameCount (gameplay counter preserved)"),
    ):
        p = _find_chunk_for_pc(chunks, pc)
        if not p:
            continue
        body = f'''    /* {MARK}: timing fallback marker {name} */
    {{
        static int moh_timing_fallback_once_{pc:08X};
        moh_pc_log_fallback_once("{name}", 0x{pc:08X}u,
                                 &moh_timing_fallback_once_{pc:08X});
    }}
'''
        count += _inject_after_label(
            p, pc, body, f"{MARK}: timing fallback marker {name}"
        )

    return count

def _patch_math(chunks: Path) -> int:
    count = 0

    for pc, (name, hostfn) in NATIVE_UNARY_DOUBLE.items():
        p = _find_chunk_for_pc(chunks, pc)
        if not p:
            continue
        body = _native_prefix(pc, name)
        body += f"    ctx->fpr[1] = (f64){hostfn}((double)ctx->fpr[1]);\n"
        body += "    ctx->ps1[1] = ctx->fpr[1];\n"
        body += "    ctx->pc = ctx->lr & ~3u;\n"
        body += "    return;\n"
        count += _inject_after_label(p, pc, body, f"{MARK}: native {name}")

    for pc, (name, hostfn) in NATIVE_BINARY_DOUBLE.items():
        p = _find_chunk_for_pc(chunks, pc)
        if not p:
            continue
        body = _native_prefix(pc, name)
        body += (
            f"    ctx->fpr[1] = (f64){hostfn}((double)ctx->fpr[1], "
            "(double)ctx->fpr[2]);\n"
        )
        body += "    ctx->ps1[1] = ctx->fpr[1];\n"
        body += "    ctx->pc = ctx->lr & ~3u;\n"
        body += "    return;\n"
        count += _inject_after_label(p, pc, body, f"{MARK}: native {name}")

    for pc, (name, hostfn) in NATIVE_UNARY_FLOAT.items():
        p = _find_chunk_for_pc(chunks, pc)
        if not p:
            continue
        body = _native_prefix(pc, name)
        body += f"    ctx->fpr[1] = (f64){hostfn}((float)ctx->fpr[1]);\n"
        body += "    ctx->ps1[1] = ctx->fpr[1];\n"
        body += "    ctx->pc = ctx->lr & ~3u;\n"
        body += "    return;\n"
        count += _inject_after_label(p, pc, body, f"{MARK}: native {name}")

    p = _find_chunk_for_pc(chunks, LDEXP_PC)
    if p:
        name = "ldexp"
        body = _native_prefix(LDEXP_PC, name)
        body += (
            "    ctx->fpr[1] = (f64)ldexp((double)ctx->fpr[1], "
            "(int)(s32)ctx->gpr[3]);\n"
        )
        body += "    ctx->ps1[1] = ctx->fpr[1];\n"
        body += "    ctx->pc = ctx->lr & ~3u;\n"
        body += "    return;\n"
        count += _inject_after_label(p, LDEXP_PC, body, f"{MARK}: native {name}")

    return count

def _patch_debug_prof(chunks: Path) -> int:
    count = 0
    for pc, name in NATIVE_VOID_NOOPS.items():
        p = _find_chunk_for_pc(chunks, pc)
        if not p:
            continue
        body = f'''    /* {MARK}: native no-op {name} */
    if (moh_pc_native_debug_prof_enabled()) {{
        static int moh_native_once_{pc:08X};
        moh_pc_log_native_once("{name}", 0x{pc:08X}u,
                               &moh_native_once_{pc:08X});
        ctx->pc = ctx->lr & ~3u;
        return;
    }}
'''
        count += _inject_after_label(
            p, pc, body, f"{MARK}: native no-op {name}"
        )
    return count


def _patch_update_lighting_native(chunks: Path) -> int:
    pc = 0x800F2CE4
    p = _find_chunk_for_pc(chunks, pc)
    if not p:
        return 0

    body = f'''    /* {MARK}: native UpdateLightingParams steady-state HLE */
    {{
        const u32 moh_r13 = ctx->gpr[13];
        const u32 moh_light_manager =
            mem_read32(ctx, moh_r13 + (u32)(s32)(-27736));
        const u8 moh_lighting_enabled =
            mem_read8(ctx, moh_r13 + (u32)(s32)(-27732));
        const u8 moh_cache_valid =
            mem_read8(ctx, moh_r13 + (u32)(s32)(-27712));

        if (moh_light_manager == 0u || moh_lighting_enabled == 0u) {{
            const u32 moh_packet = ctx->gpr[3];
            const u32 moh_dst = mem_read32(ctx, moh_packet + 8u);

            for (u32 moh_i = 0; moh_i < 24u; ++moh_i)
                mem_write32(ctx, moh_dst + moh_i * 4u, 0u);

            mem_write32(ctx, moh_packet + 8u, moh_dst + 96u);

            static int moh_native_once_disabled_800F2CE4;
            moh_pc_log_native_once(
                "UpdateLightingParams | disabled -> native zero block",
                0x800F2CE4u, &moh_native_once_disabled_800F2CE4);

            ctx->pc = ctx->lr & ~3u;
            return;
        }}

        if (moh_cache_valid != 0u) {{
            const u32 moh_packet = ctx->gpr[3];
            const u32 moh_dst = mem_read32(ctx, moh_packet + 8u);
            const u32 moh_src = 0x802DA670u;

            for (u32 moh_i = 0; moh_i < 24u; ++moh_i) {{
                const u32 moh_word =
                    mem_read32(ctx, moh_src + moh_i * 4u);
                mem_write32(ctx, moh_dst + moh_i * 4u, moh_word);
            }}

            mem_write32(ctx, moh_packet + 8u, moh_dst + 96u);

            static int moh_native_once_cached_800F2CE4;
            moh_pc_log_native_once(
                "UpdateLightingParams | cached 96-byte block",
                0x800F2CE4u, &moh_native_once_cached_800F2CE4);

            ctx->pc = ctx->lr & ~3u;
            return;
        }}

        static int moh_fallback_once_dirty_800F2CE4;
        moh_pc_log_fallback_once(
            "UpdateLightingParams | cache rebuild/matrix/light query",
            0x800F2CE4u, &moh_fallback_once_dirty_800F2CE4);
    }}
'''
    return int(_inject_after_label(
        p, pc, body, f"{MARK}: native UpdateLightingParams steady-state HLE"
    ))


def _patch_partial_logs(chunks: Path) -> int:
    count = 0
    for pc, name in PARTIALS.items():
        p = _find_chunk_for_pc(chunks, pc)
        if not p:
            continue
        body = f'''    /* {MARK}: native partial marker {name} */
    {{
        static int moh_partial_once_{pc:08X};
        moh_pc_log_partial_once("{name}", 0x{pc:08X}u,
                                &moh_partial_once_{pc:08X});
    }}
'''
        count += _inject_after_label(
            p, pc, body, f"{MARK}: native partial marker {name}"
        )
    return count

def _patch_fallback_logs(chunks: Path) -> int:
    count = 0
    for pc, name in FALLBACKS.items():
        p = _find_chunk_for_pc(chunks, pc)
        if not p:
            continue
        body = f'''    /* {MARK}: fallback marker {name} */
    {{
        static int moh_fallback_once_{pc:08X};
        moh_pc_log_fallback_once("{name}", 0x{pc:08X}u,
                                 &moh_fallback_once_{pc:08X});
    }}
'''
        count += _inject_after_label(
            p, pc, body, f"{MARK}: fallback marker {name}"
        )
    return count

def apply_gmfe69_pc_native_postgen(generated: Path) -> None:
    chunks = Path(generated) / "chunks"
    if not chunks.is_dir():
        raise RuntimeError(f"PC-native postgen: missing chunks: {chunks}")

    timing = _patch_timing(chunks)
    math = _patch_math(chunks)
    debug = _patch_debug_prof(chunks)
    lighting = _patch_update_lighting_native(chunks)
    partial = _patch_partial_logs(chunks)
    fallback = _patch_fallback_logs(chunks)

    if timing or math or debug or lighting or partial or fallback:
        print(
            "  GMFE69 PC Native: "
            f"timing={timing} math={math} debug-prof={debug} "
            f"lighting-native={lighting} partial={partial} "
            f"fallback-logged={fallback}"
        )

__all__ = ("apply_gmfe69_pc_native_postgen",)
