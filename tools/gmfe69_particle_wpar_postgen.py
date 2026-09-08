#!/usr/bin/env python3
"""GMFE69 particle GX/WPAR scalar-store fast path.

This deliberately does NOT batch GatherPipe writes.  The 36 ELF-verified
CParticleSystem::RenderSystem stores keep their original order, size and
per-store FIFO threshold checks.  We only bypass GXRuntime's generic RAM probe
for these known write-gather-pipe accesses before entering the existing Dolphin
StaticRecomp external-write hook.
"""

from __future__ import annotations

import re
from pathlib import Path

MARK = "MOH_GMFE69_PARTICLE_WPAR_DIRECT_SITE"
HELPER_MARK = "MOH_GMFE69_PARTICLE_WPAR_DIRECT_HELPER"

# Four direct GX vertices emitted by the RenderSystem hot loop.
# Each vertex is position xyz (3 x stfs), RGBA (4 x stb), UV (2 x stfs).
SITES = (
    # vertex 0
    (0x8007E64C, "stfs"), (0x8007E660, "stfs"), (0x8007E6A8, "stfs"),
    (0x8007E6B4, "stb"),  (0x8007E6C0, "stb"),  (0x8007E6C8, "stb"),
    (0x8007E6CC, "stb"),  (0x8007E6D0, "stfs"), (0x8007E6D4, "stfs"),
    # vertex 1
    (0x8007E6D8, "stfs"), (0x8007E6DC, "stfs"), (0x8007E6EC, "stfs"),
    (0x8007E6F4, "stb"),  (0x8007E700, "stb"),  (0x8007E704, "stb"),
    (0x8007E718, "stb"),  (0x8007E720, "stfs"), (0x8007E724, "stfs"),
    # vertex 2
    (0x8007E728, "stfs"), (0x8007E72C, "stfs"), (0x8007E730, "stfs"),
    (0x8007E740, "stb"),  (0x8007E748, "stb"),  (0x8007E754, "stb"),
    (0x8007E758, "stb"),  (0x8007E75C, "stfs"), (0x8007E760, "stfs"),
    # vertex 3
    (0x8007E764, "stfs"), (0x8007E768, "stfs"), (0x8007E778, "stfs"),
    (0x8007E780, "stb"),  (0x8007E790, "stb"),  (0x8007E798, "stb"),
    (0x8007E79C, "stb"),  (0x8007E7A0, "stfs"), (0x8007E7A4, "stfs"),
)

WRITE32_RE = re.compile(
    r"mem_write32\(\s*ctx\s*,\s*ea\s*,\s*(?P<value>[^;]+?)\s*\);"
)
WRITE8_RE = re.compile(
    r"mem_write8\(\s*ctx\s*,\s*ea\s*,\s*(?P<value>[^;]+?)\s*\);"
)

HELPER = r'''
/* MOH_GMFE69_PARTICLE_WPAR_DIRECT_HELPER
 *
 * RenderSystem's direct GX vertices always target the write-gather pipe.
 * mem_write{8,32} first probes MEM1 before discovering this is external MMIO.
 * In a particle storm that redundant address classification is paid 36 times
 * per quad.  Go straight to the existing StaticRecomp external-write hook for
 * verified WPAR addresses while retaining the exact scalar write order and
 * exact fallback semantics for any unexpected runtime address.
 *
 * Unlike the old Gather24 experiment this does NOT merge, delay or reorder a
 * single FIFO store, so GPFifo's normal 32-byte boundary handling and current
 * CommandProcessor backpressure remain untouched.
 */
#if defined(__GNUC__) || defined(__clang__)
#define MOH_PARTICLE_WPAR_ALWAYS_INLINE __attribute__((always_inline))
#else
#define MOH_PARTICLE_WPAR_ALWAYS_INLINE
#endif

static inline MOH_PARTICLE_WPAR_ALWAYS_INLINE
void moh_particle_wpar_write32(CPUState* cpu, u32 ea, u32 value)
{
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect((ea & 0xFFFFF000u) == 0xCC008000u &&
                         cpu->external_write != NULL, 1))
#else
    if ((ea & 0xFFFFF000u) == 0xCC008000u && cpu->external_write != NULL)
#endif
    {
        cpu->external_write(cpu, ea, value, 4);
        return;
    }

    mem_write32(cpu, ea, value);
}

static inline MOH_PARTICLE_WPAR_ALWAYS_INLINE
void moh_particle_wpar_write8(CPUState* cpu, u32 ea, u8 value)
{
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect((ea & 0xFFFFF000u) == 0xCC008000u &&
                         cpu->external_write != NULL, 1))
#else
    if ((ea & 0xFFFFF000u) == 0xCC008000u && cpu->external_write != NULL)
#endif
    {
        cpu->external_write(cpu, ea, value, 1);
        return;
    }

    mem_write8(cpu, ea, value);
}
#undef MOH_PARTICLE_WPAR_ALWAYS_INLINE

'''


def _block_bounds(text: str, pc: int) -> tuple[int, int]:
    label = f"label_{pc:08X}:"
    start = text.find(label)
    if start < 0:
        raise RuntimeError(f"particle-wpar: missing {label}")
    tail = text[start + len(label):]
    match = re.search(r"\nlabel_[0-9A-Fa-f]{8}:", tail)
    end = len(text) if not match else start + len(label) + match.start()
    return start, end


def _find_chunk_for_pc(chunks: Path, pc: int) -> Path | None:
    label = f"label_{pc:08X}:"
    matches = [
        path for path in sorted(chunks.glob("*.c"))
        if label in path.read_text(errors="ignore")
    ]
    if len(matches) > 1:
        raise RuntimeError(
            f"particle-wpar: {pc:08X} appears in {len(matches)} generated chunks"
        )
    return matches[0] if matches else None


def _ensure_helper(path: Path) -> None:
    text = path.read_text()
    if HELPER_MARK in text:
        return

    fn = re.search(
        r"(?m)^[A-Za-z_][A-Za-z0-9_ \t\*]*\bmg_img\d+_func_[0-9A-Fa-f]{8}"
        r"\s*\(CPUState\* ctx\)\s*\{",
        text,
    )
    if not fn:
        raise RuntimeError(
            f"particle-wpar: generated function anchor missing in {path.name}"
        )

    path.write_text(text[:fn.start()] + HELPER + text[fn.start():])


def _patch_site(path: Path, pc: int, kind: str) -> int:
    text = path.read_text()
    start, end = _block_bounds(text, pc)
    block = text[start:end]
    site_mark = f"{MARK}_{pc:08X}"
    if site_mark in block:
        return 0

    if kind == "stfs":
        if not re.search(
            rf"//\s*{pc:08X}:\s*stfs\s+f\d+,\s*-32768\(r31\)", block
        ):
            raise RuntimeError(
                f"particle-wpar: {pc:08X} is no longer the expected stfs WPAR store"
            )
        matches = list(WRITE32_RE.finditer(block))
        helper = "moh_particle_wpar_write32"
        cast = "u32"
    elif kind == "stb":
        if not re.search(
            rf"//\s*{pc:08X}:\s*stb\s+r\d+,\s*-32768\(r31\)", block
        ):
            raise RuntimeError(
                f"particle-wpar: {pc:08X} is no longer the expected stb WPAR store"
            )
        matches = list(WRITE8_RE.finditer(block))
        helper = "moh_particle_wpar_write8"
        cast = "u8"
    else:
        raise RuntimeError(f"particle-wpar: unsupported site kind {kind!r}")

    if len(matches) != 1:
        raise RuntimeError(
            f"particle-wpar: {pc:08X} expected one scalar mem_write, found {len(matches)}"
        )

    match = matches[0]
    value = match.group("value").strip()
    replacement = (
        f"{helper}(ctx, ea, ({cast})({value})); "
        f"/* {site_mark} */"
    )

    block = block[:match.start()] + replacement + block[match.end():]
    path.write_text(text[:start] + block + text[end:])
    return 1


def apply_gmfe69_particle_wpar_postgen(generated: Path) -> None:
    generated = Path(generated)
    chunks = generated / "chunks"
    if not chunks.is_dir():
        raise RuntimeError(
            f"particle-wpar: missing generated chunks directory: {chunks}"
        )

    # Only GMFE69's main executable image contains the verified RenderSystem PCs.
    if _find_chunk_for_pc(chunks, SITES[0][0]) is None:
        return

    site_paths: dict[int, Path] = {}
    helper_paths: set[Path] = set()
    for pc, _kind in SITES:
        path = _find_chunk_for_pc(chunks, pc)
        if path is None:
            raise RuntimeError(f"particle-wpar: missing verified WPAR site {pc:08X}")
        site_paths[pc] = path
        helper_paths.add(path)

    for path in helper_paths:
        _ensure_helper(path)

    patched = 0
    for pc, kind in SITES:
        patched += _patch_site(site_paths[pc], pc, kind)

    final_count = 0
    for path in helper_paths:
        final_count += path.read_text(errors="ignore").count(MARK + "_")

    if final_count != len(SITES):
        raise RuntimeError(
            f"particle-wpar: final marker count={final_count}, expected={len(SITES)}"
        )

    if patched:
        print(
            "  GMFE69 particle WPAR direct: "
            f"{len(SITES)} scalar FIFO stores keep exact order; RAM probe bypassed"
        )
