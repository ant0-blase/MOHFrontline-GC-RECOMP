#!/usr/bin/env python3
# Perf-guided GMFE69 hot-stack memory postgen.
#
# Targets only guest-PC ranges measured hot in perf round14. Within those
# ranges, only D-form memory instructions whose generated comment explicitly
# names (r1) are changed. Normal MEM1 stack accesses bypass the generic
# address-normalisation/callback ladder; every non-MEM1 address falls back to
# the original mem_read*/mem_write* helper.

from __future__ import annotations

import re
from pathlib import Path

HELPER_MARK = "MOH_GMFE69_HOTSTACK_HELPERS"
SITE_MARK = "MOH_GMFE69_HOTSTACK_SITE"

HOT_RANGES = (
    (0x800F34E0, 0x800F3780),
    (0x800F73C0, 0x800F7564),
    (0x80141A00, 0x80141F10),
)

LABEL_RE = re.compile(r"(?m)^label_([0-9A-Fa-f]{8}):")
STACK_COMMENT_RE = re.compile(
    r"//\s*[0-9A-Fa-f]{8}:\s*[A-Za-z0-9_.+-]+\s+.*\(r1\)"
)

READ_RE = {
    8: re.compile(r"\bmem_read8\s*\(\s*ctx\s*,\s*ea\s*\)"),
    16: re.compile(r"\bmem_read16\s*\(\s*ctx\s*,\s*ea\s*\)"),
    32: re.compile(r"\bmem_read32\s*\(\s*ctx\s*,\s*ea\s*\)"),
    64: re.compile(r"\bmem_read64\s*\(\s*ctx\s*,\s*ea\s*\)"),
}
WRITE_RE = {
    8: re.compile(r"\bmem_write8\s*\(\s*ctx\s*,\s*ea\s*,"),
    16: re.compile(r"\bmem_write16\s*\(\s*ctx\s*,\s*ea\s*,"),
    32: re.compile(r"\bmem_write32\s*\(\s*ctx\s*,\s*ea\s*,"),
    64: re.compile(r"\bmem_write64\s*\(\s*ctx\s*,\s*ea\s*,"),
}

HELPERS = r'''
/* MOH_GMFE69_HOTSTACK_HELPERS
 *
 * r1 is the GameCube stack pointer and these helpers are installed only at
 * perf-verified r1-relative instructions. The common MEM1 path uses one
 * unsigned bounds check and a direct big-endian access. Any unusual address
 * retains the original generic memory semantics.
 */
#if defined(__GNUC__) || defined(__clang__)
#define MOH_HOTSTACK_INLINE __attribute__((always_inline))
#define MOH_HOTSTACK_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define MOH_HOTSTACK_INLINE
#define MOH_HOTSTACK_LIKELY(x) (x)
#endif

static inline MOH_HOTSTACK_INLINE bool
moh_hotstack_mem1(CPUState* cpu, u32 addr, u32 width, u32* out_offset)
{
    const u32 offset = addr - GC_RAM_BASE;
    if (MOH_HOTSTACK_LIKELY(cpu->ram_size >= width &&
                            offset <= cpu->ram_size - width))
    {
        *out_offset = offset;
        return true;
    }
    return false;
}

static inline MOH_HOTSTACK_INLINE u8
moh_hotstack_read8(CPUState* cpu, u32 addr)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 1u, &offset))
        return cpu->ram[offset];
    return mem_read8(cpu, addr);
}

static inline MOH_HOTSTACK_INLINE u16
moh_hotstack_read16(CPUState* cpu, u32 addr)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 2u, &offset))
    {
        const u8* p = cpu->ram + offset;
        return ((u16)p[0] << 8) | (u16)p[1];
    }
    return mem_read16(cpu, addr);
}

static inline MOH_HOTSTACK_INLINE u32
moh_hotstack_read32(CPUState* cpu, u32 addr)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 4u, &offset))
    {
        const u8* p = cpu->ram + offset;
        return ((u32)p[0] << 24) |
               ((u32)p[1] << 16) |
               ((u32)p[2] << 8) |
               (u32)p[3];
    }
    return mem_read32(cpu, addr);
}

static inline MOH_HOTSTACK_INLINE u64
moh_hotstack_read64(CPUState* cpu, u32 addr)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 8u, &offset))
    {
        const u8* p = cpu->ram + offset;
        return ((u64)p[0] << 56) |
               ((u64)p[1] << 48) |
               ((u64)p[2] << 40) |
               ((u64)p[3] << 32) |
               ((u64)p[4] << 24) |
               ((u64)p[5] << 16) |
               ((u64)p[6] << 8) |
               (u64)p[7];
    }
    return mem_read64(cpu, addr);
}

static inline MOH_HOTSTACK_INLINE void
moh_hotstack_write8(CPUState* cpu, u32 addr, u8 value)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 1u, &offset))
    {
        clear_matching_reservation(cpu, addr);
        GXRUNTIME_JOURNAL_WRITE(offset, 1u);
        cpu->ram[offset] = value;
        return;
    }
    mem_write8(cpu, addr, value);
}

static inline MOH_HOTSTACK_INLINE void
moh_hotstack_write16(CPUState* cpu, u32 addr, u16 value)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 2u, &offset))
    {
        clear_matching_reservation(cpu, addr);
        GXRUNTIME_JOURNAL_WRITE(offset, 2u);
        u8* p = cpu->ram + offset;
        p[0] = (u8)(value >> 8);
        p[1] = (u8)value;
        return;
    }
    mem_write16(cpu, addr, value);
}

static inline MOH_HOTSTACK_INLINE void
moh_hotstack_write32(CPUState* cpu, u32 addr, u32 value)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 4u, &offset))
    {
        clear_matching_reservation(cpu, addr);
        GXRUNTIME_JOURNAL_WRITE(offset, 4u);
        u8* p = cpu->ram + offset;
        p[0] = (u8)(value >> 24);
        p[1] = (u8)(value >> 16);
        p[2] = (u8)(value >> 8);
        p[3] = (u8)value;
        return;
    }
    mem_write32(cpu, addr, value);
}

static inline MOH_HOTSTACK_INLINE void
moh_hotstack_write64(CPUState* cpu, u32 addr, u64 value)
{
    u32 offset;
    if (moh_hotstack_mem1(cpu, addr, 8u, &offset))
    {
        clear_matching_reservation(cpu, addr);
        GXRUNTIME_JOURNAL_WRITE(offset, 8u);
        u8* p = cpu->ram + offset;
        p[0] = (u8)(value >> 56);
        p[1] = (u8)(value >> 48);
        p[2] = (u8)(value >> 40);
        p[3] = (u8)(value >> 32);
        p[4] = (u8)(value >> 24);
        p[5] = (u8)(value >> 16);
        p[6] = (u8)(value >> 8);
        p[7] = (u8)value;
        return;
    }
    mem_write64(cpu, addr, value);
}

#undef MOH_HOTSTACK_LIKELY
#undef MOH_HOTSTACK_INLINE
'''


def _pc_hot(pc: int) -> bool:
    return any(lo <= pc <= hi for lo, hi in HOT_RANGES)


def _contains_hot_pc(chunks: Path) -> bool:
    for path in sorted(chunks.glob("*.c")):
        text = path.read_text(errors="ignore")
        for match in LABEL_RE.finditer(text):
            if _pc_hot(int(match.group(1), 16)):
                return True
    return False


def _inject_helpers(text: str) -> str:
    if HELPER_MARK in text:
        return text
    anchor = '#include "../generated.h"\n'
    if anchor not in text:
        raise RuntimeError("hotstack postgen: generated.h include anchor missing")
    return text.replace(anchor, anchor + "\n" + HELPERS + "\n", 1)


def _patch_block(block: str) -> tuple[str, dict[str, int]]:
    counts = {f"r{w}": 0 for w in (8, 16, 32, 64)}
    counts.update({f"w{w}": 0 for w in (8, 16, 32, 64)})

    if SITE_MARK in block or not STACK_COMMENT_RE.search(block):
        return block, counts

    for width, pattern in READ_RE.items():
        repl = f"moh_hotstack_read{width}(ctx, ea) /* {SITE_MARK} */"
        block, n = pattern.subn(repl, block)
        counts[f"r{width}"] += n

    for width, pattern in WRITE_RE.items():
        # Keep the original value argument untouched.
        repl = f"moh_hotstack_write{width}(ctx, ea, /* {SITE_MARK} */"
        block, n = pattern.subn(repl, block)
        counts[f"w{width}"] += n

    return block, counts


def _patch_text(text: str) -> tuple[str, dict[str, int], list[int]]:
    labels = list(LABEL_RE.finditer(text))
    totals = {f"r{w}": 0 for w in (8, 16, 32, 64)}
    totals.update({f"w{w}": 0 for w in (8, 16, 32, 64)})
    patched_pcs: list[int] = []

    edits: list[tuple[int, int, str]] = []
    for i, match in enumerate(labels):
        pc = int(match.group(1), 16)
        if not _pc_hot(pc):
            continue
        start = match.start()
        end = labels[i + 1].start() if i + 1 < len(labels) else len(text)
        block = text[start:end]
        patched, counts = _patch_block(block)
        n = sum(counts.values())
        if not n:
            continue
        edits.append((start, end, patched))
        patched_pcs.append(pc)
        for key, value in counts.items():
            totals[key] += value

    if not edits:
        return text, totals, patched_pcs

    for start, end, patched in reversed(edits):
        text = text[:start] + patched + text[end:]

    text = _inject_helpers(text)
    return text, totals, patched_pcs


def apply_gmfe69_hotstack_postgen(generated: Path) -> None:
    chunks = Path(generated) / "chunks"
    if not chunks.is_dir():
        raise RuntimeError(f"hotstack postgen: missing chunks directory: {chunks}")

    grand = {f"r{w}": 0 for w in (8, 16, 32, 64)}
    grand.update({f"w{w}": 0 for w in (8, 16, 32, 64)})
    pcs: list[int] = []
    files = 0

    for path in sorted(chunks.glob("*.c")):
        text = path.read_text()
        patched, counts, file_pcs = _patch_text(text)
        if patched == text:
            continue
        path.write_text(patched)
        files += 1
        pcs.extend(file_pcs)
        for key, value in counts.items():
            grand[key] += value

    total = sum(grand.values())
    if total == 0:
        existing = 0
        for path in sorted(chunks.glob("*.c")):
            existing += path.read_text(errors="ignore").count(SITE_MARK)
        if existing:
            print(f"  GMFE69 hot-stack MEM1 fastpath: already present ({existing} markers)")
            return

        if not _contains_hot_pc(chunks):
            print("  GMFE69 hot-stack MEM1 fastpath: skip (target PCs absent in this image)")
            return

        print(
            "  GMFE69 hot-stack MEM1 fastpath: "
            "0 patchable r1 sites in target PCs; leaving generated code unchanged"
        )
        return

    if total < 8:
        print(
            "  GMFE69 hot-stack MEM1 fastpath: "
            f"only {total} site(s) matched; keeping conservative partial optimization"
        )

    unique_pcs = sorted(set(pcs))
    summary = " ".join(f"{k}={v}" for k, v in grand.items() if v)
    print(
        "  GMFE69 hot-stack MEM1 fastpath: "
        f"{total} sites / {len(unique_pcs)} guest PCs / {files} chunks "
        f"({summary})"
    )
    if unique_pcs:
        print(
            "    PCs: " +
            ", ".join(f"{pc:08X}" for pc in unique_pcs[:24]) +
            (" ..." if len(unique_pcs) > 24 else "")
        )
