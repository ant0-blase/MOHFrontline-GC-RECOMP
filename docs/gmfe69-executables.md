# GMFE69 executable layout

The USA GameCube build of **Medal of Honor: Frontline** uses more than the disc boot DOL. The multi-image module builder therefore discovers every `.dol` and `.elf` under `extracted/` and recompiles every unique executable image into one `gGMFE69_recomp.so`.

The currently inspected GMFE69 dump contains four executable paths but only three unique binaries:

| Image | Runtime source | Entry point | Native code ranges | Notes |
|---|---|---:|---|---|
| 0 | `sys/main.dol` | `0x80680040` | `0x80680000-0x80682500`, `0x80682600-0x806A5FA0` | Disc boot/loader |
| alias | `files/Moh2BootRel.dol` | `0x80680040` | same as image 0 | Byte-identical to `sys/main.dol`; compiled once, not duplicated |
| 1 | `files/Moh2RelGC.elf` | `0x80007140` | `0x80007100-0x800095EC`, `0x80016E80-0x8016DF40` | Main game executable |
| 2 | `files/Moh2StubRelGC.elf` | `0x80680040` | `0x80680000-0x806824E4`, `0x80682600-0x806A5F68` | Restart/loader stub; overlaps the boot DOL |

## Why ABI v5 is required

The boot DOL and `Moh2StubRelGC.elf` deliberately reuse the same GameCube virtual addresses while containing different instructions. They therefore cannot be merged into one ordinary address-to-function table.

The module uses canonical address chunks plus an accepted hash set for each chunk. When the game loader writes a new executable image and invalidates the instruction cache, ModernGekko hashes the guest bytes. ABI v5 calls `select_chunk_variant(chunk, hash)` and only enables the native variant whose build-time bytes match exactly.

This gives the runtime the intended sequence without falling back merely because the executable image changed:

```text
boot DOL @ 0x8068....
        |
        +--> loads Moh2RelGC.elf @ 0x8000....
        |
        +--> restart path loads Moh2StubRelGC.elf @ 0x8068....
                         ^ same addresses, different verified image
```

A hash that matches none of the executable images is rejected and the normal StaticRecomp safety/fallback path remains in control.

## ELF conversion

DolRecomp currently consumes DOL containers directly. `tools/elf2dol.py` therefore creates a temporary, address-preserving DOL representation of each ELF for code generation. It maps ELF `PT_LOAD` segments at their original PowerPC virtual addresses and keeps data load segments too.

The temporary DOL is **not** used by the game at runtime. The original ELF remains in `extracted/files/` and the original MOH loader continues to load it exactly as the game expects.

## Build-time report

Every successful build writes:

```text
module/multi-image-report.json
```

It records the discovered unique images, aliases, SHA-256 values, entry points, code ranges, generated chunk counts, canonical chunk count, and overlap count. Use `./tools/inspect_game.sh` to summarize it.

## Recovered ELF function symbols

The two runtime ELF images are non-stripped and retain usable `.symtab` and
`.strtab` sections. The inspected USA files expose 5,005 named executable
functions in `Moh2RelGC.elf` and 660 in `Moh2StubRelGC.elf`.

`tools/elf_symbols.py` extracts only defined `STT_FUNC` symbols that belong to
executable ELF sections. During a normal build, those symbols are converted to
DolRecomp MAP files and supplied with `--map`, producing address/size constants
in each image's `generated_symbols.h` as well as JSON/CSV inventories under
`module/symbols/`.

This metadata is especially useful for turning a guest PC such as `0x80018CD8`
into the original function name (`GameLoop__Fv`) when debugging crashes or
profiling native coverage. The generated C chunks remain implementation chunks;
they are not one-to-one with the 5,665 guest functions, so the original ELF
symbol map is kept as authoritative function metadata instead of renaming chunk
functions incorrectly.
