#!/usr/bin/env python3
"""Convert a 32-bit big-endian PowerPC ELF executable into a DOL container.

This is intentionally a code-generation container, not a replacement for the
game's ELF loader. PT_LOAD segments are mapped to DOL text/data sections at
their original virtual addresses so DolRecomp can consume the executable while
the real game continues to load the original ELF at runtime.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

DOL_TEXT_MAX = 7
DOL_DATA_MAX = 11
DOL_HEADER_SIZE = 0x100
PT_LOAD = 1
PF_X = 1


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def parse_elf(path: Path):
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")
    if data[4] != 1:
        raise ValueError(f"{path}: only ELF32 is supported")
    if data[5] != 2:
        raise ValueError(f"{path}: only big-endian ELF is supported")

    endian = ">"
    machine = struct.unpack_from(endian + "H", data, 18)[0]
    if machine != 20:  # EM_PPC
        raise ValueError(f"{path}: expected PowerPC ELF (e_machine=20), got {machine}")

    entry = struct.unpack_from(endian + "I", data, 24)[0]
    phoff = struct.unpack_from(endian + "I", data, 28)[0]
    phentsize = struct.unpack_from(endian + "H", data, 42)[0]
    phnum = struct.unpack_from(endian + "H", data, 44)[0]

    text = []
    other = []
    for i in range(phnum):
        off = phoff + i * phentsize
        if off + 32 > len(data):
            raise ValueError(f"{path}: truncated program header {i}")
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
            endian + "IIIIIIII", data, off
        )
        if p_type != PT_LOAD or p_filesz == 0:
            continue
        if p_offset + p_filesz > len(data):
            raise ValueError(f"{path}: PT_LOAD {i} extends past EOF")
        seg = {
            "index": i,
            "vaddr": p_vaddr,
            "filesz": p_filesz,
            "memsz": p_memsz,
            "flags": p_flags,
            "align": p_align,
            "bytes": data[p_offset : p_offset + p_filesz],
        }
        (text if (p_flags & PF_X) else other).append(seg)

    text.sort(key=lambda s: s["vaddr"])
    other.sort(key=lambda s: s["vaddr"])
    if not text:
        raise ValueError(f"{path}: no executable PT_LOAD segments")
    if len(text) > DOL_TEXT_MAX:
        raise ValueError(f"{path}: {len(text)} executable PT_LOAD segments exceed DOL limit {DOL_TEXT_MAX}")
    if len(other) > DOL_DATA_MAX:
        raise ValueError(f"{path}: {len(other)} data PT_LOAD segments exceed DOL limit {DOL_DATA_MAX}")
    return entry, text, other


def build_dol(entry: int, text, other) -> bytes:
    header = bytearray(DOL_HEADER_SIZE)
    payload = bytearray()
    cursor = DOL_HEADER_SIZE

    def emit_segment(seg, slot: int, is_text: bool):
        nonlocal cursor
        aligned = align_up(cursor, 0x20)
        if aligned > DOL_HEADER_SIZE + len(payload):
            payload.extend(b"\0" * (aligned - (DOL_HEADER_SIZE + len(payload))))
        cursor = aligned
        file_off = cursor
        payload.extend(seg["bytes"])
        cursor += len(seg["bytes"])

        offset_base = 0x00 if is_text else 0x1C
        address_base = 0x48 if is_text else 0x64
        size_base = 0x90 if is_text else 0xAC
        struct.pack_into(">I", header, offset_base + slot * 4, file_off)
        struct.pack_into(">I", header, address_base + slot * 4, seg["vaddr"])
        struct.pack_into(">I", header, size_base + slot * 4, seg["filesz"])

    for i, seg in enumerate(text):
        emit_segment(seg, i, True)
    for i, seg in enumerate(other):
        emit_segment(seg, i, False)

    # Converted DOLs are only fed to DolRecomp; the original ELF remains the
    # runtime loader source of truth. Leave DOL BSS empty rather than inventing
    # one coarse range across ELF's multiple NOBITS segments.
    struct.pack_into(">I", header, 0xD8, 0)
    struct.pack_into(">I", header, 0xDC, 0)
    struct.pack_into(">I", header, 0xE0, entry)
    return bytes(header + payload)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()

    entry, text, other = parse_elf(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_dol(entry, text, other))
    print(f"ELF -> DOL: {args.input} -> {args.output}")
    print(f"entry: 0x{entry:08X}; text segments: {len(text)}; data segments: {len(other)}")
    for seg in text:
        print(f"  RX 0x{seg['vaddr']:08X}-0x{seg['vaddr'] + seg['filesz']:08X} ({seg['filesz']:#x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
