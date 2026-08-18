#!/usr/bin/env python3
"""Extract named PowerPC function symbols from a non-stripped ELF32 image.

GMFE69 ships non-stripped ELF executables.  This helper reads their ELF symbol
and string tables directly (no binutils dependency) and can emit:

* a DolRecomp-compatible MAP:  <address> <size> <name>
* JSON/CSV inventories for reverse engineering and runtime debugging

Only defined STT_FUNC symbols that live in executable sections are exported.
"""
from __future__ import annotations

import argparse
import csv
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2MSB = 2
EM_PPC = 20
SHT_SYMTAB = 2
SHT_DYNSYM = 11
SHF_EXECINSTR = 0x4
SHN_UNDEF = 0
STT_FUNC = 2

BINDING_NAMES = {
    0: "LOCAL",
    1: "GLOBAL",
    2: "WEAK",
}

VISIBILITY_NAMES = {
    0: "DEFAULT",
    1: "INTERNAL",
    2: "HIDDEN",
    3: "PROTECTED",
}


@dataclass(frozen=True)
class FunctionSymbol:
    address: int
    size: int
    name: str
    binding: str
    visibility: str
    section: str

    def json_dict(self) -> dict:
        d = asdict(self)
        d["address"] = f"0x{self.address:08X}"
        d["size_hex"] = f"0x{self.size:X}"
        return d


def _cstr(blob: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(blob):
        return ""
    end = blob.find(b"\0", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("utf-8", errors="replace")


def extract_function_symbols(path: Path) -> list[FunctionSymbol]:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != ELF_MAGIC:
        raise ValueError(f"{path}: not an ELF file")
    if data[4] != ELFCLASS32:
        raise ValueError(f"{path}: only ELF32 is supported")
    if data[5] != ELFDATA2MSB:
        raise ValueError(f"{path}: only big-endian ELF is supported")

    machine = struct.unpack_from(">H", data, 18)[0]
    if machine != EM_PPC:
        raise ValueError(f"{path}: expected PowerPC ELF (e_machine={EM_PPC}), got {machine}")

    shoff = struct.unpack_from(">I", data, 32)[0]
    shentsize = struct.unpack_from(">H", data, 46)[0]
    shnum = struct.unpack_from(">H", data, 48)[0]
    shstrndx = struct.unpack_from(">H", data, 50)[0]
    if shentsize < 40 or shoff + shentsize * shnum > len(data):
        raise ValueError(f"{path}: invalid/truncated section table")

    sections: list[dict] = []
    for i in range(shnum):
        off = shoff + i * shentsize
        fields = struct.unpack_from(">IIIIIIIIII", data, off)
        sec = {
            "index": i,
            "name_off": fields[0],
            "type": fields[1],
            "flags": fields[2],
            "addr": fields[3],
            "offset": fields[4],
            "size": fields[5],
            "link": fields[6],
            "info": fields[7],
            "align": fields[8],
            "entsize": fields[9],
            "name": "",
        }
        if sec["offset"] + sec["size"] > len(data) and sec["type"] != 8:  # SHT_NOBITS
            raise ValueError(f"{path}: section {i} extends past EOF")
        sections.append(sec)

    if shstrndx >= len(sections):
        raise ValueError(f"{path}: invalid section-name string table index")
    shstr = sections[shstrndx]
    shstr_blob = data[shstr["offset"] : shstr["offset"] + shstr["size"]]
    for sec in sections:
        sec["name"] = _cstr(shstr_blob, sec["name_off"])

    out: list[FunctionSymbol] = []
    seen: set[tuple[int, int, str, int]] = set()
    for symtab in sections:
        if symtab["type"] not in (SHT_SYMTAB, SHT_DYNSYM):
            continue
        if symtab["link"] >= len(sections):
            continue
        strtab = sections[symtab["link"]]
        strings = data[strtab["offset"] : strtab["offset"] + strtab["size"]]
        entsize = symtab["entsize"] or 16
        if entsize < 16:
            continue
        count = symtab["size"] // entsize
        for i in range(count):
            off = symtab["offset"] + i * entsize
            if off + 16 > len(data):
                break
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(">IIIBBH", data, off)
            if (st_info & 0xF) != STT_FUNC or st_shndx == SHN_UNDEF:
                continue
            if st_shndx >= len(sections):
                continue
            owner = sections[st_shndx]
            if not (owner["flags"] & SHF_EXECINSTR):
                continue
            name = _cstr(strings, st_name)
            if not name:
                continue
            key = (st_value, st_size, name, st_shndx)
            if key in seen:
                continue
            seen.add(key)
            out.append(
                FunctionSymbol(
                    address=st_value,
                    size=st_size,
                    name=name,
                    binding=BINDING_NAMES.get(st_info >> 4, f"BIND_{st_info >> 4}"),
                    visibility=VISIBILITY_NAMES.get(st_other & 0x3, f"VIS_{st_other & 0x3}"),
                    section=owner["name"] or f"section_{st_shndx}",
                )
            )

    # Keep aliases at the same address, but make output deterministic and put
    # stronger/global names before local aliases when addresses collide.
    binding_rank = {"GLOBAL": 0, "WEAK": 1, "LOCAL": 2}
    out.sort(key=lambda s: (s.address, binding_rank.get(s.binding, 3), s.name, s.size))
    return out


def write_map(path: Path, symbols: list[FunctionSymbol]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Auto-extracted from ELF .symtab/.strtab for DolRecomp --map\n")
        for sym in symbols:
            f.write(f"0x{sym.address:08X} 0x{sym.size:08X} {sym.name}\n")


def write_json(path: Path, input_path: Path, symbols: list[FunctionSymbol], source_label: str | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "source": source_label if source_label is not None else str(input_path),
        "function_count": len(symbols),
        "functions": [s.json_dict() for s in symbols],
    }
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def write_csv(path: Path, symbols: list[FunctionSymbol]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "size", "name", "binding", "visibility", "section"])
        for s in symbols:
            w.writerow([f"0x{s.address:08X}", f"0x{s.size:X}", s.name, s.binding, s.visibility, s.section])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("--map", dest="map_path", type=Path)
    ap.add_argument("--json", dest="json_path", type=Path)
    ap.add_argument("--csv", dest="csv_path", type=Path)
    ap.add_argument("--count", action="store_true", help="print only the function-symbol count")
    args = ap.parse_args()

    symbols = extract_function_symbols(args.input)
    if args.map_path:
        write_map(args.map_path, symbols)
    if args.json_path:
        write_json(args.json_path, args.input, symbols)
    if args.csv_path:
        write_csv(args.csv_path, symbols)

    if args.count:
        print(len(symbols))
    else:
        print(f"{args.input}: {len(symbols)} named executable function symbols")
        if args.map_path:
            print(f"  DolRecomp MAP: {args.map_path}")
        if args.json_path:
            print(f"  JSON:          {args.json_path}")
        if args.csv_path:
            print(f"  CSV:           {args.csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
