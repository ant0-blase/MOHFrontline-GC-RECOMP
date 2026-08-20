#!/usr/bin/env python3
"""Build one ModernGekko native module containing every GMFE69 DOL/ELF image.

ELF files are converted to address-preserving DOL containers solely for
DolRecomp. The original game still loads the original ELF files at runtime.
Overlapping executables are retained as hash-selected variants, so the boot DOL
and Moh2StubRelGC.elf can safely reuse 0x8068.... in one native module.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from gmfe69_postgen import apply_gmfe69_generated_postgen, apply_gmfe69_export_postgen
from elf_symbols import (
    FunctionSymbol,
    extract_function_symbols,
    write_csv as write_symbol_csv,
    write_json as write_symbol_json,
    write_map as write_symbol_map,
)

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def run(cmd, *, cwd=None):
    print("+", " ".join(str(x) for x in cmd), flush=True)
    subprocess.run([str(x) for x in cmd], cwd=cwd, check=True)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def fnv1a64(data: bytes) -> int:
    h = FNV64_OFFSET
    for b in data:
        h = ((h ^ b) * FNV64_PRIME) & MASK64
    return h


@dataclass
class DolImage:
    path: Path
    entry: int
    text_sections: list[tuple[int, int, int]]  # address, size, file offset
    raw: bytes

    @classmethod
    def load(cls, path: Path) -> "DolImage":
        raw = path.read_bytes()
        if len(raw) < 0x100:
            raise ValueError(f"{path}: truncated DOL")

        def be32(off: int) -> int:
            return struct.unpack_from(">I", raw, off)[0]

        text = []
        for i in range(7):
            file_off = be32(0x00 + 4 * i)
            address = be32(0x48 + 4 * i)
            size = be32(0x90 + 4 * i)
            if file_off and address and size:
                if file_off + size > len(raw):
                    raise ValueError(f"{path}: text{i} past EOF")
                text.append((address, size, file_off))
        if not text:
            raise ValueError(f"{path}: no DOL text sections")
        return cls(path, be32(0xE0), text, raw)

    def read_text(self, start: int, end: int) -> bytes:
        for address, size, file_off in self.text_sections:
            if address <= start and end <= address + size:
                lo = file_off + (start - address)
                return self.raw[lo : lo + (end - start)]
        raise ValueError(
            f"{self.path}: [0x{start:08X},0x{end:08X}) is not inside one text section"
        )


@dataclass
class ImageBuild:
    index: int
    source: Path
    source_rel: str
    source_kind: str
    source_sha256: str
    compile_dol: Path
    dol: DolImage
    generated: Path
    adapter: Path
    code_ranges: list[tuple[int, int]] = field(default_factory=list)
    chunk_ranges: list[tuple[int, int]] = field(default_factory=list)
    smc_ranges: list[tuple[int, int]] = field(default_factory=list)
    aliases: list[str] = field(default_factory=list)
    symbol_count: int = 0
    symbol_map: Path | None = None
    symbol_json: Path | None = None
    symbol_csv: Path | None = None
    symbol_header: Path | None = None
    symbols: list[FunctionSymbol] = field(default_factory=list)


def parse_generated_header(path: Path):
    text = path.read_text()
    code = {
        (int(a, 16), int(b, 16))
        for a, b in re.findall(
            r"address >= (0x[0-9A-Fa-f]+)u && address < (0x[0-9A-Fa-f]+)u", text
        )
    }
    for base, span in re.findall(
        r"u32\s+offset\s*=\s*address\s*-\s*(0x[0-9A-Fa-f]+)u\s*;\s*"
        r"if\s*\(\s*offset\s*<\s*(0x[0-9A-Fa-f]+)u",
        text,
    ):
        start = int(base, 16)
        code.add((start, start + int(span, 16)))
    code_ranges = sorted(code)
    if not code_ranges:
        raise ValueError(f"no code ranges found in {path}")

    funcs = sorted(
        int(a, 16)
        for a in re.findall(r"void func_([0-9A-Fa-f]{8})\(CPUState\* ctx\);", text)
    )
    if not funcs:
        raise ValueError(f"no generated functions found in {path}")

    chunks = []
    for i, addr in enumerate(funcs):
        containing = next(((a, b) for a, b in code_ranges if a <= addr < b), None)
        if containing is None:
            raise ValueError(f"func_{addr:08X} outside generated code ranges")
        end = containing[1]
        if i + 1 < len(funcs) and containing[0] <= funcs[i + 1] < containing[1]:
            end = funcs[i + 1]
        chunks.append((addr, end))
    return code_ranges, chunks


def parse_smc(path: Path):
    ranges = []
    if not path.exists():
        return ranges
    for line in path.read_text().splitlines():
        m = re.match(r"\s*(0x[0-9A-Fa-f]+)-(0x[0-9A-Fa-f]+)", line)
        if m:
            ranges.append((int(m.group(1), 16), int(m.group(2), 16) + 4))
    return ranges


def merge_ranges(ranges):
    if not ranges:
        return []
    out = []
    for start, end in sorted(ranges):
        if start >= end:
            continue
        if out and start <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], end))
        else:
            out.append((start, end))
    return out


def prefix_generated_functions(generated: Path, prefix: str):
    rx = re.compile(r"\bfunc_([0-9A-Fa-f]{8})\b")
    targets = [generated / "generated.h", *sorted((generated / "chunks").glob("*.c"))]
    for path in targets:
        text = path.read_text()
        text = rx.sub(lambda m: f"{prefix}func_{m.group(1)}", text)
        path.write_text(text)




def _find_chunk_with_label(generated: Path, label: str) -> Path:
    needle = f"label_{label}:"
    matches = []
    for path in sorted((generated / "chunks").glob("*.c")):
        if needle in path.read_text():
            matches.append(path)
    if len(matches) != 1:
        raise ValueError(
            f"GMFE69 enhancement patch expected exactly one {needle}, found {len(matches)}"
        )
    return matches[0]


def _ensure_moh_include(path: Path):
    text = path.read_text()
    include = '#include "moh_frontline_enhancements.h"\n'
    if include in text:
        return
    anchor = '#include "../generated.h"\n'
    if anchor not in text:
        raise ValueError(f"GMFE69 enhancement patch cannot find generated include in {path}")
    path.write_text(text.replace(anchor, anchor + include, 1))


def _inject_after_label(generated: Path, label: str, body: str, occurrence: str = "unique"):
    path = _find_chunk_with_label(generated, label)
    _ensure_moh_include(path)
    text = path.read_text()
    anchor = f"label_{label}:\n"
    if body.strip() in text:
        return
    count = text.count(anchor)
    if occurrence == "unique":
        if count != 1:
            raise ValueError(f"GMFE69 enhancement patch cannot uniquely patch {anchor} in {path}")
        text = text.replace(anchor, anchor + body, 1)
    elif occurrence == "first":
        if count < 1:
            raise ValueError(f"GMFE69 enhancement patch cannot find {anchor} in {path}")
        text = text.replace(anchor, anchor + body, 1)
    elif occurrence == "last":
        if count < 1:
            raise ValueError(f"GMFE69 enhancement patch cannot find {anchor} in {path}")
        pos = text.rfind(anchor)
        text = text[:pos] + anchor + body + text[pos + len(anchor):]
    else:
        raise ValueError(f"invalid injection occurrence: {occurrence}")
    path.write_text(text)


def apply_gmfe69_generated_enhancements(generated: Path):
    """Inject GMFE69 overrides into the actual generated C fast paths."""

    _inject_after_label(generated, "80079E7C", """    if (moh_camera_override(ctx)) {
        ctx->pc = ctx->lr & ~3u;
        goto return_dispatch_80076E80;
    }
""")

    # The player weapon/viewmodel builds its own Perspective matrix and never
    # goes through CCamera::SetPerspective.  Adjust its already-computed tangent
    # pair immediately before each Perspective call.
    for weapon_proj_label in ("800A8458", "800A84A8"):
        _inject_after_label(generated, weapon_proj_label, """    (void)moh_weapon_projection_override(ctx);
""")

    # EA VP6 playback is synchronous. Enter movie-only precise sampling / 4:3
    # at RCMP_PlayMovie entry and restore the exact v10.2 graphics state at its
    # common epilogue. This avoids the global Manual Texture Sampling penalty.
    _inject_after_label(generated, "8010408C", """    if (ctx->host_call)
        (void)ctx->host_call(ctx, 0xFFFFF140u);
""")
    _inject_after_label(generated, "8010420C", """    if (ctx->host_call)
        (void)ctx->host_call(ctx, 0xFFFFF141u);
""")

    # Native PC mouse look.  Inject before BeginUpdate's epilogue, while r31
    # still contains the live CPlayerObject* (mr r31,r3 at function entry).
    # 0x800A4CE8 is too late: lmw at 0x800A4CD8 has already restored r31.
    _inject_after_label(generated, "800A4CB4", """    if (ctx->host_call)
        (void)ctx->host_call(ctx, 0xFFFFF120u);
""")

    # CoD-style FPS ADS presentation.  UpdateWeaponTransforms has r30=this and
    # the temporary viewmodel translation vector at guest stack+0x30 by this
    # point. The host modifies only that temporary vector, never animation data.
    _inject_after_label(generated, "800A8920", """    if (ctx->host_call)
        (void)ctx->host_call(ctx, 0xFFFFF130u);
""")

    # GetCrosshairStatus is just lbz r3,0x215(r3); blr.  Let the host clear r3
    # while fully ADS without bypassing or replacing the original HUD logic.
    _inject_after_label(generated, "800DF23C", """    if (ctx->host_call)
        (void)ctx->host_call(ctx, 0xFFFFF132u);
""")

    # Frontend IStudio/UIS.  Its authored coordinate space is native 640x480;
    # apply only aspect compensation around 320,240.  HUD scale/safe-width are
    # deliberately not applied to menus.
    _inject_after_label(generated, "8008B6C8", """    moh_ui_begin(ctx);
""")
    _inject_after_label(generated, "8008B6D4", """    moh_ui_end(ctx);
""")

    # Gameplay HUD is a completely separate renderer: UserInterface::Draw
    # submits 640x480 spritepolyvert arrays plus CFont text directly. Scope the
    # draw so only gameplay HUD primitives receive HUD scale/safe-area changes.
    _inject_after_label(generated, "800BEB24", """    moh_hud_begin(ctx);
""")
    _inject_after_label(generated, "800BEE20", """    moh_hud_end(ctx);
""")

    # Scale the actual transient HUD polygon vertices, then restore the source
    # array after RenderList has consumed it. This changes both position and
    # sprite size instead of merely moving a still-stretched sprite.
    _inject_after_label(generated, "8008240C", """    moh_hud_poly_begin(ctx);
""")
    _inject_after_label(generated, "80082460", """    moh_hud_poly_end(ctx);
""")

    # CFont is independent of CMatrixStack.  UIS fonts inherit frontend scale;
    # gameplay HUD fonts receive HUD scale and corrected 640x480 positions.
    _inject_after_label(generated, "8007CB1C", """    moh_ui_font_scale_override(ctx);
""")
    _inject_after_label(generated, "8007CF50", """    moh_hud_text_position_override(ctx);
""")
    _inject_after_label(generated, "8007CDB0", """    moh_hud_centered_text_position_override(ctx);
""")
    _inject_after_label(generated, "8007CBB0", """    moh_hud_centered_text_position_override(ctx);
""")

    # Arm high-rate VI only for actual in-level gameplay.  The shell/menu and
    # LoadTheGame paths must keep original GameCube timing because their state
    # machines and DVD/streaming waits are VBlank-sensitive.
    _inject_after_label(generated, "80018378", """    moh_timing_set_gameplay(ctx, 1);
""")
    _inject_after_label(generated, "80018570", """    moh_timing_set_gameplay(ctx, 0);
""")
    _inject_after_label(generated, "80019794", """    moh_timing_set_gameplay(ctx, 0);
""")

    # Keep the original CScreen::Wait VBlank synchronization.  ModernGekko's
    # native VI-frequency override supplies faster VBlanks; only advance MOH's
    # virtual 60-Hz simulation clock after the original wait has completed.
    _inject_after_label(generated, "8001B938", """    if (moh_timing_enabled()) {
        moh_timing_frame_advance();
    }
""")
    _inject_after_label(generated, "8001B940", """    if (moh_timing_enabled()) {
        ctx->gpr[4] = moh_timing_vsyncs(ctx);
        goto label_8001B944;
    }
""")
    _inject_after_label(generated, "8001B95C", """    if (moh_timing_enabled()) {
        ctx->fpr[1] = moh_timing_delta_ticks();
        goto label_8001B960;
    }
""")
    _inject_after_label(generated, "8001BA40", """    if (moh_timing_enabled()) {
        ctx->gpr[3] = moh_timing_integer_ticks();
        goto label_8001BA44;
    }
""")

    # Every caller that converts CScreen::Wait()'s integer return into the
    # shared floating frame delta must use the fractional host delta.  This
    # covers gameplay plus status/restart/error-screen loops.
    for entry, store in (
        ("80017D20", "80017D3C"),
        ("800181C4", "800181D8"),
        ("800184B0", "800184C4"),
        ("800187C4", "800187D8"),
        ("800189C4", "800189D8"),
        ("80019D50", "80019D6C"),
        ("80019FD8", "80019FF4"),
        ("8001A1F0", "8001A20C"),
    ):
        _inject_after_label(generated, entry, f"""    if (moh_timing_enabled()) {{
        ctx->fpr[0] = moh_timing_delta_ticks();
        goto label_{store};
    }}
""")

    _inject_after_label(generated, "80017E2C", """    if (moh_timing_enabled()) {
        ctx->fpr[1] = moh_timing_game_time_seconds();
        ctx->pc = ctx->lr & ~3u;
        goto return_dispatch_80016E80;
    }
""")
    _inject_after_label(generated, "80017E60", """    if (moh_timing_enabled()) {
        ctx->gpr[3] = moh_timing_frame_count();
        ctx->pc = ctx->lr & ~3u;
        goto return_dispatch_80016E80;
    }
""")
    _inject_after_label(generated, "8001B908", """    if (moh_timing_enabled()) {
        ctx->gpr[3] = moh_timing_vsyncs(ctx);
        ctx->pc = ctx->lr & ~3u;
        goto return_dispatch_8001AE80;
    }
""")

    print("  GMFE69 native enhancements injected into DolRecomp C")


def write_adapter(image: ImageBuild, prefix: str):
    adapter = image.generated / f"adapter_img{image.index}.c"
    adapter.write_text(
        f'''#include "generated.h"\n\n'''
        f'''int mg_image_{image.index}_dispatch(CPUState* ctx, u32 address)\n{{\n'''
        f'''    return dolrecomp_call(ctx, address);\n}}\n'''
    )
    image.adapter = adapter


def discover_executables(extracted: Path):
    files = []
    main = extracted / "sys" / "main.dol"
    if not main.is_file():
        raise ValueError(f"missing {main}")
    files.append(main)
    for path in sorted(extracted.rglob("*")):
        if not path.is_file() or path == main:
            continue
        if path.suffix.lower() in (".dol", ".elf"):
            files.append(path)
    return files


def write_tables(work: Path, images: list[ImageBuild], entry_point: int):
    union_code = merge_ranges([r for image in images for r in image.code_ranges])
    if not union_code:
        raise ValueError("no native code ranges")

    # Every generated-chunk boundary becomes a canonical verification boundary.
    all_boundaries = {x for image in images for r in image.chunk_ranges for x in r}
    canonical = []
    for code_start, code_end in union_code:
        bounds = sorted({code_start, code_end, *[x for x in all_boundaries if code_start < x < code_end]})
        for a, b in zip(bounds, bounds[1:]):
            if a < b:
                canonical.append((a, b))

    variants_per_chunk: list[list[tuple[int, int]]] = []
    for start, end in canonical:
        candidates = []
        seen = set()
        for image in images:
            if any(a <= start and end <= b for a, b in image.code_ranges):
                runtime_hash = fnv1a64(image.dol.read_text(start, end))
                key = (runtime_hash, image.index)
                if key not in seen:
                    seen.add(key)
                    candidates.append(key)
        if not candidates:
            raise ValueError(f"no image owns canonical chunk 0x{start:08X}-0x{end:08X}")
        # Hash duplicates are semantically interchangeable for this interval;
        # keep one deterministic image to reduce the selector table.
        unique_by_hash = {}
        for h, idx in candidates:
            unique_by_hash.setdefault(h, idx)
        variants_per_chunk.append(sorted((h, idx) for h, idx in unique_by_hash.items()))

    smc = merge_ranges([r for image in images for r in image.smc_ranges])
    table_path = work / "module_tables.inc"
    flat_variants = []
    offsets = [0]
    for variants in variants_per_chunk:
        flat_variants.extend(variants)
        offsets.append(len(flat_variants))

    with table_path.open("w") as f:
        f.write("// Generated multi-image module tables — do not edit.\n")
        f.write("static const StaticRecompRange s_code_ranges[] = {\n")
        for a, b in union_code:
            f.write(f"    {{0x{a:08X}u, 0x{b:08X}u}},\n")
        f.write("};\n")
        f.write(f"#define MODULE_CODE_RANGE_COUNT {len(union_code)}u\n")
        f.write("static const StaticRecompRange s_smc_ranges[] = {\n")
        if smc:
            for a, b in smc:
                f.write(f"    {{0x{a:08X}u, 0x{b:08X}u}},\n")
        else:
            f.write("    {0u, 0u},\n")
        f.write("};\n")
        f.write(f"#define MODULE_SMC_RANGE_COUNT {len(smc)}u\n")
        f.write("static const StaticRecompRange s_chunk_ranges[] = {\n")
        for a, b in canonical:
            f.write(f"    {{0x{a:08X}u, 0x{b:08X}u}},\n")
        f.write("};\n")
        f.write(f"#define MODULE_CHUNK_RANGE_COUNT {len(canonical)}u\n")

        # Fast page hint table for the native dispatcher. The table stores the
        # first canonical chunk that overlaps each 4 KiB guest page. The C
        # lookup then checks only the one or two chunk boundaries that can
        # share that page instead of binary-searching the whole table.
        page_shift = 12
        page_size = 1 << page_shift
        page_base = canonical[0][0] & ~(page_size - 1)
        page_end = (canonical[-1][1] + page_size - 1) & ~(page_size - 1)
        page_count = (page_end - page_base) >> page_shift
        page_hints = []
        chunk_i = 0
        for page in range(page_count):
            start = page_base + page * page_size
            end = start + page_size
            while chunk_i < len(canonical) and canonical[chunk_i][1] <= start:
                chunk_i += 1
            if chunk_i < len(canonical) and canonical[chunk_i][0] < end:
                page_hints.append(chunk_i)
            else:
                page_hints.append(0xFFFF)

        f.write(f"#define MULTI_CHUNK_PAGE_SHIFT {page_shift}u\n")
        f.write(f"#define MULTI_CHUNK_PAGE_BASE 0x{page_base:08X}u\n")
        f.write(f"#define MULTI_CHUNK_PAGE_COUNT {page_count}u\n")
        f.write("#define MULTI_CHUNK_PAGE_INVALID 0xFFFFu\n")
        f.write("static const u16 s_chunk_page_hint[] = {\n")
        for i in range(0, len(page_hints), 16):
            row = page_hints[i:i + 16]
            f.write("    " + ", ".join(f"{x}u" for x in row) + ",\n")
        f.write("};\n")

        # Zero hashes are intentional: ABI v5 select_chunk_variant validates
        # against the complete accepted-hash table below instead.
        f.write("static const u64 s_chunk_hashes[] = {\n")
        for _ in canonical:
            f.write("    0u,\n")
        f.write("};\n")
        f.write("typedef struct MultiVariantHash { u64 hash; u32 image; } MultiVariantHash;\n")
        f.write("static const MultiVariantHash s_variant_hashes[] = {\n")
        for h, idx in flat_variants:
            f.write(f"    {{0x{h:016X}ull, {idx}u}},\n")
        f.write("};\n")
        f.write("static const u32 s_variant_offsets[] = {\n")
        for x in offsets:
            f.write(f"    {x}u,\n")
        f.write("};\n")
        f.write(f"#define MODULE_ENTRY_POINT 0x{entry_point:08X}u\n")

    return union_code, canonical, variants_per_chunk, smc


def write_export(work: Path, images: list[ImageBuild]):
    p = work / "multi_module_export.c"
    decls = "\n".join(
        f"int mg_image_{image.index}_dispatch(CPUState* ctx, u32 address);" for image in images
    )
    switches = "\n".join(
        f"        case {image.index}u: return mg_image_{image.index}_dispatch(ctx, address);"
        for image in images
    )
    burst_switches = "\n".join(
        f"            case {image.index}u: dispatched = mg_image_{image.index}_dispatch(ctx, pc); break;"
        for image in images
    )
    p.write_text(
        f'''#include "StaticRecompABI.h"\n#include <stdio.h>\n\n{decls}\n\n'''
        '''#include "module_tables.inc"\n\n'''
        '''#define MULTI_INVALID_IMAGE 0xFFFFFFFFu\n'''
        '''static u32 s_active_image[MODULE_CHUNK_RANGE_COUNT];\n'''
        '''static int s_initialized = 0;\n\n'''
        '''static void multi_init_once(void)\n{\n'''
        '''    if (s_initialized) return;\n'''
        '''    for (u32 i = 0; i < MODULE_CHUNK_RANGE_COUNT; ++i) s_active_image[i] = MULTI_INVALID_IMAGE;\n'''
        '''    s_initialized = 1;\n}\n\n'''
        '''static int multi_chunk_index(u32 address)\n{\n'''
        '''    const u32 delta = address - MULTI_CHUNK_PAGE_BASE;\n'''
        '''    const u32 page = delta >> MULTI_CHUNK_PAGE_SHIFT;\n'''
        '''    if (page >= MULTI_CHUNK_PAGE_COUNT) return -1;\n'''
        '''    u32 chunk = s_chunk_page_hint[page];\n'''
        '''    if (chunk == MULTI_CHUNK_PAGE_INVALID) return -1;\n'''
        '''    /* A page can straddle a generated chunk boundary (or one of\n'''
        '''     * the tiny overlap splits). Start from its first candidate and\n'''
        '''     * walk only until the range start passes the requested PC. */\n'''
        '''    for (; chunk < MODULE_CHUNK_RANGE_COUNT; ++chunk) {\n'''
        '''        if (address < s_chunk_ranges[chunk].start) break;\n'''
        '''        if (address < s_chunk_ranges[chunk].end) return (int)chunk;\n'''
        '''    }\n'''
        '''    return -1;\n}\n\n'''
        '''static int multi_select_chunk_variant(u32 chunk_index, u64 runtime_hash)\n{\n'''
        '''    multi_init_once();\n'''
        '''    if (chunk_index >= MODULE_CHUNK_RANGE_COUNT) return 0;\n'''
        '''    const u32 begin = s_variant_offsets[chunk_index];\n'''
        '''    const u32 end = s_variant_offsets[chunk_index + 1u];\n'''
        '''    for (u32 i = begin; i < end; ++i) {\n'''
        '''        if (s_variant_hashes[i].hash == runtime_hash) {\n'''
        '''            const u32 old_image = s_active_image[chunk_index];\n'''
        '''            s_active_image[chunk_index] = s_variant_hashes[i].image;\n'''
        '''            if (old_image != s_active_image[chunk_index]) {\n'''
        '''                fprintf(stderr, "[staticrecomp] multi-image chunk %u -> image %u [0x%08X,0x%08X)\\n",\n'''
        '''                        chunk_index, s_active_image[chunk_index],\n'''
        '''                        s_chunk_ranges[chunk_index].start, s_chunk_ranges[chunk_index].end);\n'''
        '''            }\n'''
        '''            return 1;\n'''
        '''        }\n'''
        '''    }\n'''
        '''    s_active_image[chunk_index] = MULTI_INVALID_IMAGE;\n'''
        '''    return 0;\n}\n\n'''
        '''#if defined(__GNUC__) || defined(__clang__)\n'''
        '''__attribute__((noinline))\n'''
        '''#endif\n'''
        '''static u64 chassis_div_u64_u32(u64 value, u32 divisor)\n{\n'''
        '''    return value / divisor;\n'''
        '''}\n\n'''
        '''static int chassis_dispatch(CPUState* ctx, u32 address)\n{\n'''
        '''    multi_init_once();\n'''
        '''    const int chunk = multi_chunk_index(address);\n'''
        '''    if (chunk < 0) return 0;\n'''
        '''    switch (s_active_image[(u32)chunk]) {\n'''
        f'''{switches}\n'''
        '''        default: return 0;\n'''
        '''    }\n}\n\n'''
        '''static u32 chassis_dispatch_burst(\n'''
        '''    CPUState* ctx,\n'''
        '''    u32 address,\n'''
        '''    const u8* chain_state,\n'''
        '''    u32 chain_state_count,\n'''
        '''    u64 cycle_budget,\n'''
        '''    u64 timebase_origin,\n'''
        '''    u64 timebase_cycles_before,\n'''
        '''    u32 timebase_ratio)\n{\n'''
        '''    multi_init_once();\n'''
        '''    if (!chain_state || chain_state_count == 0u || cycle_budget == 0u) return 0u;\n'''
        '''    u32 blocks = 0u;\n'''
        '''    u64 total_cycles = 0u;\n'''
        '''    int cached_chunk = -1;\n'''
        '''    u32 cached_chunk_start = 0u;\n'''
        '''    u32 cached_chunk_end = 0u;\n'''
        '''    ctx->pc = address;\n'''
        '''    while (blocks < 64u && total_cycles < cycle_budget) {\n'''
        '''        const u32 pc = ctx->pc;\n'''
        '''        int chunk = cached_chunk;\n'''
        '''        if (chunk < 0 || pc < cached_chunk_start || pc >= cached_chunk_end) {\n'''
        '''            chunk = multi_chunk_index(pc);\n'''
        '''            if (chunk < 0) break;\n'''
        '''            cached_chunk = chunk;\n'''
        '''            cached_chunk_start = s_chunk_ranges[(u32)chunk].start;\n'''
        '''            cached_chunk_end = s_chunk_ranges[(u32)chunk].end;\n'''
        '''        }\n'''
        '''        if ((u32)chunk >= chain_state_count || chain_state[(u32)chunk] == 0u) break;\n'''
        '''        const u32 begin = s_variant_offsets[(u32)chunk];\n'''
        '''        const u32 end = s_variant_offsets[(u32)chunk + 1u];\n'''
        '''        /* Never burst through an address range shared by multiple executable images. */\n'''
        '''        if (end - begin != 1u) break;\n'''
        '''        const u32 image = s_active_image[(u32)chunk];\n'''
        '''        if (image == MULTI_INVALID_IMAGE) break;\n'''
        '''        ctx->downcount = 0;\n'''
        '''        int dispatched = 0;\n'''
        '''        switch (image) {\n'''
        f'''{burst_switches}\n'''
        '''            default: break;\n'''
        '''        }\n'''
        '''        if (!dispatched) break;\n'''
        '''        const s64 raw_charge = -ctx->downcount;\n'''
        '''        const u64 charge = raw_charge > 0 ? (u64)raw_charge : 1u;\n'''
        '''        total_cycles += charge;\n'''
        '''        ++blocks;\n'''
        '''        if (timebase_ratio != 0u) {\n'''
        '''            const u64 tb_cycles = timebase_cycles_before + total_cycles;\n'''
        '''            u64 tb_delta;\n'''
        '''#if defined(__GNUC__) || defined(__clang__)\n'''
        '''            if (__builtin_expect(timebase_ratio == 12u, 1))\n'''
        '''#else\n'''
        '''            if (timebase_ratio == 12u)\n'''
        '''#endif\n'''
        '''                tb_delta = tb_cycles / 12u;\n'''
        '''            else\n'''
        '''                tb_delta = chassis_div_u64_u32(tb_cycles, timebase_ratio);\n'''
        '''            ctx->timebase = timebase_origin + tb_delta;\n'''
        '''        }\n'''
        '''        ctx->downcount = 0;\n'''
        '''        if (ctx->exception) break;\n'''
        '''    }\n'''
        '''    ctx->downcount = -(s64)total_cycles;\n'''
        '''    return blocks;\n'''
        '''}\n\n'''
        '''static void chassis_on_state_loaded(CPUState* ctx)\n{\n'''
        '''    ppc_fpscr_updated(ctx);\n'''
        '''}\n\n'''
        '''static const StaticRecompModuleDesc s_desc = {\n'''
        '''    STATICRECOMP_ABI_VERSION,\n'''
        '''    GXRUNTIME_CPU_ABI_VERSION,\n'''
        '''    (u32)sizeof(CPUState),\n'''
        '''    MODULE_GAME_ID,\n'''
        '''    MODULE_ENTRY_POINT,\n'''
        '''    chassis_dispatch,\n'''
        '''    chassis_on_state_loaded,\n'''
        '''    s_code_ranges, MODULE_CODE_RANGE_COUNT,\n'''
        '''    s_smc_ranges, MODULE_SMC_RANGE_COUNT,\n'''
        '''    s_chunk_ranges, MODULE_CHUNK_RANGE_COUNT,\n'''
        '''    s_chunk_hashes,\n'''
        '''    0, 0,\n'''
        '''    chassis_dispatch_burst, /* unique-image chunks only */\n'''
        '''    multi_select_chunk_variant,\n'''
        '''};\n\n'''
        '''#if defined(_WIN32)\n#define RECOMP_MODULE_EXPORT __declspec(dllexport)\n'''
        '''#elif defined(__GNUC__) || defined(__clang__)\n#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))\n'''
        '''#else\n#define RECOMP_MODULE_EXPORT\n#endif\n\n'''
        '''RECOMP_MODULE_EXPORT const StaticRecompModuleDesc* staticrecomp_get_module(void)\n{\n'''
        '''    multi_init_once();\n'''
        '''    return &s_desc;\n}\n'''
    )
    return p


def cmake_quote(path: Path) -> str:
    return str(path).replace("\\", "/")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--extracted", type=Path, required=True)
    ap.add_argument("--dolrecomp", type=Path, required=True)
    ap.add_argument("--project-root", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--game-id", default="GMFE69")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--backend", default="c")
    ap.add_argument("--compiler", default="clang")
    ap.add_argument("--opt-level", default="0")
    ap.add_argument("--cmake-toolchain", type=Path,
                    help="Optional CMake toolchain file for cross compilation")
    ap.add_argument("--module-type", choices=("SHARED", "STATIC"), default="SHARED",
                    help="Build the recompiled game image as a shared or static library")
    ap.add_argument("--cmake-define", action="append", default=[], metavar="KEY=VALUE",
                    help="Extra -D definition forwarded to the module CMake configure step")
    args = ap.parse_args()

    if args.backend != "c":
        raise SystemExit("multi-image build currently requires BACKEND=c (symbol-prefixing C chunks)")

    root = args.project_root.resolve()
    extracted = args.extracted.resolve()
    dolrecomp = args.dolrecomp.resolve()
    if not dolrecomp.is_file():
        raise SystemExit(f"DolRecomp executable not found: {dolrecomp}")
    if not os.access(dolrecomp, os.X_OK):
        raise SystemExit(f"DolRecomp is not executable: {dolrecomp}")
    args.dolrecomp = dolrecomp
    work = args.work.resolve()
    shutil.rmtree(work, ignore_errors=True)
    (work / "images").mkdir(parents=True)
    (work / "containers").mkdir(parents=True)
    symbols_dir = work / "symbols"
    symbols_dir.mkdir(parents=True)

    discovered = discover_executables(extracted)
    print(f"discovered {len(discovered)} DOL/ELF paths")

    # Exact duplicate binaries are represented once, with aliases retained in
    # the manifest. GMFE69 has sys/main.dol == files/Moh2BootRel.dol.
    unique = []
    by_hash = {}
    aliases = {}
    for path in discovered:
        digest = sha256(path)
        rel = path.relative_to(extracted).as_posix()
        if digest in by_hash:
            aliases.setdefault(by_hash[digest], []).append(rel)
            print(f"  duplicate: {rel} == {by_hash[digest]}")
            continue
        by_hash[digest] = rel
        unique.append((path, rel, digest))

    elf2dol = root / "tools" / "elf2dol.py"
    images: list[ImageBuild] = []
    for idx, (source, rel, digest) in enumerate(unique):
        kind = source.suffix.lower().lstrip(".")
        image_dir = work / "images" / f"img{idx:02d}"
        generated_parent = image_dir / "dolrecomp-output"
        image_dir.mkdir(parents=True)
        image_symbols: list[FunctionSymbol] = []
        symbol_map_path = None
        symbol_json_path = None
        symbol_csv_path = None
        if kind == "elf":
            compile_dol = work / "containers" / f"img{idx:02d}-{source.stem}.dol"
            run([sys.executable, elf2dol, source, compile_dol])

            # GMFE69's Moh2RelGC.elf and Moh2StubRelGC.elf are non-stripped.
            # Recover their STT_FUNC symbols and feed them back to DolRecomp so
            # generated_symbols.h contains real CodeWarrior function names.
            image_symbols = extract_function_symbols(source)
            if image_symbols:
                stem = f"img{idx:02d}-{source.stem}"
                symbol_map_path = symbols_dir / f"{stem}.functions.map"
                symbol_json_path = symbols_dir / f"{stem}.functions.json"
                symbol_csv_path = symbols_dir / f"{stem}.functions.csv"
                write_symbol_map(symbol_map_path, image_symbols)
                write_symbol_json(symbol_json_path, source, image_symbols, source_label=rel)
                write_symbol_csv(symbol_csv_path, image_symbols)
                print(f"  symbols: {rel}: {len(image_symbols)} named functions")
        elif kind == "dol":
            compile_dol = source
        else:
            continue

        dol = DolImage.load(compile_dol)
        dolrecomp_cmd = [
            args.dolrecomp,
            f"-j{args.jobs}",
            "--backend=c",
            "--cpu", "gekko",
            "--gamecube",
        ]
        if symbol_map_path is not None:
            dolrecomp_cmd += ["--map", symbol_map_path]
        dolrecomp_cmd += [compile_dol, generated_parent]
        run(dolrecomp_cmd)
        generated = generated_parent / "generated"
        header = generated / "generated.h"
        if not header.is_file():
            raise ValueError(f"DolRecomp did not produce {header}")
        code_ranges, chunk_ranges = parse_generated_header(header)
        smc_ranges = parse_smc(generated / "generated_smc.txt")
        prefix = f"mg_img{idx}_"
        prefix_generated_functions(generated, prefix)
        if args.game_id == "GMFE69" and rel.lower().endswith("moh2relgc.elf"):
            apply_gmfe69_generated_enhancements(generated)
        image = ImageBuild(
            index=idx,
            source=source,
            source_rel=rel,
            source_kind=kind,
            source_sha256=digest,
            compile_dol=compile_dol,
            dol=dol,
            generated=generated,
            adapter=generated / "unused.c",
            code_ranges=code_ranges,
            chunk_ranges=chunk_ranges,
            smc_ranges=smc_ranges,
            aliases=aliases.get(rel, []),
            symbol_count=len(image_symbols),
            symbol_map=symbol_map_path,
            symbol_json=symbol_json_path,
            symbol_csv=symbol_csv_path,
            symbols=image_symbols,
        )
        generated_symbol_header = generated / "generated_symbols.h"
        if generated_symbol_header.is_file():
            published_header = symbols_dir / f"img{idx:02d}-{source.stem}.generated_symbols.h"
            shutil.copy2(generated_symbol_header, published_header)
            image.symbol_header = published_header
        if args.game_id == "GMFE69":
            apply_gmfe69_generated_postgen(generated)
        write_adapter(image, prefix)
        images.append(image)

    if not images:
        raise SystemExit("no executable images were generated")

    main_image = next((x for x in images if x.source_rel == "sys/main.dol"), images[0])
    union_code, canonical, variants, smc = write_tables(work, images, main_image.dol.entry)
    export_source = write_export(work, images)
    if args.game_id == "GMFE69":
        apply_gmfe69_export_postgen(export_source)

    manifest = work / "multi_manifest.cmake"
    chunk_inputs = [p for image in images for p in sorted((image.generated / "chunks").glob("*.c"))]
    with manifest.open("w") as f:
        f.write(f'set(MULTI_WORK_DIR "{cmake_quote(work)}")\n')
        f.write(f'set(MULTI_EXPORT_SOURCE "{cmake_quote(export_source)}")\n')
        f.write("set(MULTI_ADAPTER_INPUTS\n")
        for image in images:
            f.write(f'  "{cmake_quote(image.adapter)}"\n')
        f.write(")\nset(MULTI_CHUNK_INPUTS\n")
        for path in chunk_inputs:
            f.write(f'  "{cmake_quote(path)}"\n')
        f.write(")\n")

    report = {
        "game_id": args.game_id,
        "entry_point": f"0x{main_image.dol.entry:08X}",
        "images": [
            {
                "index": image.index,
                "source": image.source_rel,
                "aliases": image.aliases,
                "kind": image.source_kind,
                "sha256": image.source_sha256,
                "entry_point": f"0x{image.dol.entry:08X}",
                "code_ranges": [[f"0x{a:08X}", f"0x{b:08X}"] for a, b in image.code_ranges],
                "generated_chunks": len(image.chunk_ranges),
                "function_symbols": image.symbol_count,
                "symbol_map": image.symbol_map.name if image.symbol_map else None,
                "symbol_json": image.symbol_json.name if image.symbol_json else None,
                "symbol_csv": image.symbol_csv.name if image.symbol_csv else None,
                "generated_symbols_header": image.symbol_header.name if image.symbol_header else None,
            }
            for image in images
        ],
        "function_symbols_total": sum(image.symbol_count for image in images),
        "module_code_ranges": [[f"0x{a:08X}", f"0x{b:08X}"] for a, b in union_code],
        "canonical_chunks": len(canonical),
        "variant_hash_entries": sum(len(v) for v in variants),
        "overlap_chunks": sum(1 for v in variants if len(v) > 1),
        "burst_safe_chunks": sum(1 for v in variants if len(v) == 1),
        "smc_ranges": len(smc),
    }
    (work / "multi-image-report.json").write_text(json.dumps(report, indent=2) + "\n")

    # One combined inventory makes address/name lookups easy in Ghidra, logs,
    # scripts and issue reports while retaining per-image maps for DolRecomp.
    combined = {
        "game_id": args.game_id,
        "function_count": sum(image.symbol_count for image in images),
        "images": [
            {
                "index": image.index,
                "source": image.source_rel,
                "function_count": image.symbol_count,
                "functions": [sym.json_dict() for sym in image.symbols],
            }
            for image in images if image.symbols
        ],
    }
    (symbols_dir / "function-symbols.json").write_text(
        json.dumps(combined, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    with (symbols_dir / "function-symbols.csv").open("w", encoding="utf-8", newline="") as f:
        import csv
        writer = csv.writer(f)
        writer.writerow(["image", "source", "address", "size", "name", "binding", "visibility", "section"])
        for image in images:
            for sym in image.symbols:
                writer.writerow([
                    image.index, image.source_rel, f"0x{sym.address:08X}", f"0x{sym.size:X}",
                    sym.name, sym.binding, sym.visibility, sym.section,
                ])

    module_build = work / "module-build"
    template = root / "multi-module-template"
    gxruntime = root / "ModernGekko" / "vendor" / "dolphin" / "GXRuntime"
    abi = root / "ModernGekko" / "vendor" / "dolphin" / "Source" / "Core" / "Core" / "PowerPC" / "StaticRecomp"
    compiler = shutil.which(args.compiler) or args.compiler
    cmake_command = [
        "cmake", "-S", template, "-B", module_build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DGAME_ID={args.game_id}",
        f"-DMULTI_MANIFEST={manifest}",
        f"-DGXRUNTIME_DIR={gxruntime}",
        f"-DCHASSIS_ABI_DIR={abi}",
        f"-DRECOMPCORE_MODULE_OPT_LEVEL={args.opt_level}",
        f"-DRECOMP_MODULE_TYPE={args.module_type}",
    ]
    if args.cmake_toolchain:
        cmake_command.append(f"-DCMAKE_TOOLCHAIN_FILE={args.cmake_toolchain.resolve()}")
    else:
        cmake_command.append(f"-DCMAKE_C_COMPILER={compiler}")
    for definition in args.cmake_define:
        if "=" not in definition or definition.startswith("="):
            raise SystemExit(f"invalid --cmake-define {definition!r}; expected KEY=VALUE")
        cmake_command.append(f"-D{definition}")
    run(cmake_command)
    run(["cmake", "--build", module_build, "-j", str(args.jobs)])

    module_stem = f"g{args.game_id}_recomp"
    candidates = []
    for suffix in (".so", ".dll", ".dylib", ".a", ".lib"):
        candidates.extend(module_build.rglob(module_stem + suffix))
    candidates = [path for path in candidates if path.is_file()]
    if not candidates:
        raise SystemExit(
            f"module build did not produce {module_stem} with a supported library suffix")
    # Prefer the artifact matching the requested module type when a platform
    # generator emits import libraries next to a DLL.
    if args.module_type == "STATIC":
        preferred = [p for p in candidates if p.suffix.lower() in (".a", ".lib")]
    else:
        preferred = [p for p in candidates if p.suffix.lower() in (".so", ".dll", ".dylib")]
    built = (preferred or candidates)[0]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, args.output)
    print(f"multi-image module: {args.output}")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
