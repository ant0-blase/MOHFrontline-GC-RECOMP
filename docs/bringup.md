# GMFE69 bring-up notes

## 1. Confirm the executable inventory

```bash
./tools/inspect_game.sh
```

For the inspected USA build, expect three unique executable images in the module:

- image 0: `sys/main.dol` (`files/Moh2BootRel.dol` is an exact duplicate alias);
- image 1: `files/Moh2RelGC.elf`;
- image 2: `files/Moh2StubRelGC.elf`.

After building, inspect:

```bash
cat module/build-info.txt
cat module/multi-image-report.json
```

## 2. First native launch

```bash
STATICRECOMP_NATIVE_BURST=0 ./run.sh 2>&1 | tee moh-frontline-firstboot.log
```

Do not add fallback ranges pre-emptively.

Watch for `multi-image chunk ... -> image N` lines. They show which verified executable owns an address range at that moment. Seeing image 1 confirms entry into the main game ELF; seeing image 2 confirms the overlapping restart/stub ELF was recognized as a different native image.

## 3. If a code image changes

ABI v5 expects I-cache invalidation to retire the affected canonical chunks to `UNVERIFIED`. The next entry hashes guest RAM and selects a known variant. An unknown hash must not be accepted as native code.

Useful symptoms:

- `image 0 -> image 2` around `0x8068....`: expected boot/stub transition;
- hash mismatch immediately after an ELF load: inspect loader completion/I-cache invalidation boundaries;
- execution outside all module code ranges: identify whether it is another DOL/ELF not discovered by the build or genuinely generated/self-modifying code.

## 4. Crash/hang capture

Record:

- last guest PowerPC PC/address;
- current selected image if logged;
- whether failure is boot, intro, menu, mission load or gameplay;
- unsupported opcode/MMIO/GX/audio/DVD/exception messages;
- whether it reproduces with `STATICRECOMP_NATIVE_BURST=0`.

For a proven narrow problem only:

```bash
STATICRECOMP_FALLBACK_RANGES=START-END ./run.sh
```

Then bisect and remove the fallback rather than keeping a broad interpreter island.

## 5. Milestones

- [x] Discover every GMFE69 DOL/ELF
- [x] Deduplicate identical boot DOL alias
- [x] Recompile main game ELF
- [x] Recompile overlapping stub ELF
- [x] Link all unique images into one `gGMFE69_recomp.so`
- [x] ABI v5 hash-selected overlap table
- [x] Direct module ABI/selector validation
- [ ] Runtime loads module on target machine
- [ ] Boot -> main ELF native transition
- [ ] First frame
- [ ] EA / intro sequence
- [ ] Main menu
- [ ] New game / mission load
- [ ] D-Day gameplay
- [ ] Audio
- [ ] Controller input
- [ ] Save/load
- [ ] Restart -> stub ELF native transition
- [ ] Stable level transitions
- [x] Optimized O2 build
- [x] Safe native burst for non-overlapping multi-image chunks
- [ ] CPU profiling / hot-path optimization
