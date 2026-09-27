# Changelog

All notable changes to **MOHFrontline-GC-RECOMP** are documented here.

This project is experimental. Release notes describe the state of the recompilation/runtime and do not imply that original retail game data or remaster assets are distributed with the project.

## [Unreleased]

## [v0.0.2] - 2026-09-27

Windows input and PS3 remaster asset hotfix release.

### Windows input

- Fixed the standalone Win32 mouse event path so the PC settings/ImGui overlay receives cursor position, mouse buttons and wheel input correctly.
- Kept Raw Input available for gameplay camera/look while separating it from absolute UI pointer input.
- Expanded Win32 keyboard translation for navigation, modifiers and function keys used by the standalone runtime.
- Fixes the Windows behavior where the settings UI could be opened but could not be clicked and where keyboard/mouse input was only partially functional.

### PS3 remaster assets

- Preserved per-level path scope for package-backed PS3 assets instead of flattening lookups that lost the current level context.
- Added GameCube `level.viv` access through NativeVFS/encounter-nod for MSH and DMF signature pairing.
- Direct ISO/GCM/RVZ/etc. launches can now provide the original GC level data needed to resolve corresponding PS3 mesh/material assets without requiring a fully extracted GameCube filesystem.
- Improves lookup of PS3 textures, MSH, DMF and related level-scoped remaster data when using either `PS3_FILES` or a finalized PS3 `.pkg`.

### Release

- Rebuilds the portable Windows and Linux packages from the corrected runtime.
- No retail GameCube data or PS3 remaster assets are included.

## [v0.0.1] - 2026-09-27

First public pre-alpha release of the native PC recompilation of the USA GameCube release of **Medal of Honor: Frontline** (`GMFE69`).

### Static recompilation core

- Added PowerPC-to-native C recompilation through **DolRecomp** and the ModernGekko runtime.
- Recompiles the complete executable chain used by GMFE69 instead of treating the game as a single `main.dol`:
  - `sys/main.dol` / `files/Moh2BootRel.dol` — boot/loader;
  - `files/Moh2RelGC.elf` — main game executable;
  - `files/Moh2StubRelGC.elf` — restart/loader stub.
- Detects exact duplicate executable images so `Moh2BootRel.dol` can alias `sys/main.dol` instead of being compiled twice.
- Added **multi-image ABI v5** for executable images that overlap in GameCube address space.
- Added runtime executable-byte hashing so overlapping boot/stub addresses dispatch to the native implementation that actually owns the current guest code.
- Unknown executable hashes fail closed instead of being dispatched to the wrong native function.
- Added recursive DOL/ELF discovery and one combined `gGMFE69_recomp` native module.
- Added non-stripped ELF symbol recovery and publication of JSON, CSV, MAP and generated symbol headers.
- Recovers approximately **5,665 named executable functions** from the main and stub ELF images.
- Added CodeWarrior-style MAP/symbol integration to preserve useful original function names in generated code.
- Native module is built at **O3** by default.
- Added permanent GMFE69 post-generation transformations so validated generated-code optimizations survive clean rebuilds.
- No GMFE69-wide interpreter fallback range is enabled by default.

### CPU and dispatcher optimizations

- Added native cache-control helpers and generic SPR handling instead of routing those operations through the interpreter fallback.
- Added overlap-safe native burst dispatch where executable ownership is unambiguous.
- Added cached chunk lookup inside burst dispatch.
- Added direct image dispatch to avoid redundant binary searches.
- Added GameCube timebase specialization for the GMFE69 timing path.
- Added x86-64 hardware FMA support where applicable.
- Added the GMFE69 idle-loop override at `0x80115F64`.
- Reduced CP gather-pipe and blocking-loop hot-path overhead.
- Added hot FP-availability specialization.
- Added particle renderer LFD reconstruction and validated MEM1 write fast paths.
- Added GameCube MEM1-only and physical-MEM1 alias handling to the generated module runtime.

### ModernGekko runtime

- Added the ModernGekko production chassis around the statically recompiled game code.
- Provides GameCube memory/address-space behavior, exceptions, GX/video, input, timing, DVD/filesystem services, audio and executable-image switching.
- Keeps ModernGekko/Dolphin compatibility paths available as explicit fallbacks while native/recompiled paths are progressively taken over.
- Added production runtime builds for both **Linux x86_64** and **Windows x86_64**.
- Added Windows-specific fixes for the native runtime, including MSVC warning-clean production builds and native-video state handling when optional FFmpeg support is unavailable.

### Extraction-less disc access

- Integrated **encounter/nod** for direct GameCube disc-image access.
- Supported source containers:
  - ISO / GCM;
  - RVZ;
  - WIA;
  - WBFS;
  - CISO;
  - GCZ;
  - TGC;
  - NFS.
- The launcher validates the selected GameCube image and requires the supported USA disc ID `GMFE69`.
- Added a minimal AOT disc cache containing only the system metadata and executable containers needed by DolRecomp.
- The complete retail filesystem no longer has to be extracted for the portable build path.
- Added direct runtime boot from the original selected disc image.
- Added a nod-backed ModernGekko `DiscSource` for logical DI reads.
- Added a nod-backed FST VFS with file-by-path and range reads.
- Added the **`GC-disc/nod`** NativeVFS source so original GameCube assets can be read directly from ISO/RVZ/etc. rather than requiring `extracted/files`.
- Added `MOH_NATIVE_DISC_IMAGE` plumbing on Linux and Windows to connect the launcher-selected image to the native VFS.

### Native PC asset/runtime paths

- Added host-side native VFS plumbing with GameCube, direct-disc and optional PS3 asset sources.
- Added native audio/video/render integration points with ModernGekko fallback behavior where the native path is not available.
- Added direct GameCube host-asset reads for developer/extracted builds.
- Added extraction-less GameCube asset reads for portable disc-image builds.
- Added native PC settings and input integration used by the recompilation runtime.

### Widescreen, FOV and frame controls

- Added original 4:3 presentation plus:
  - 16:10;
  - 16:9;
  - 21:9;
  - 32:9;
  - arbitrary `WIDTHxHEIGHT` aspect ratios;
  - automatic aspect detection where host output information is available.
- Added configurable horizontal FOV.
- Added separate weapon-FOV control.
- Added configurable frame targets from 1 to 120 FPS.
- Added `--fps 0` to remove the explicit target limiter.
- Original aspect ratio, FOV and timing behavior remain available.

### Live PC graphics settings

- Added a live in-game PC settings interface accessible with **Ctrl+F10** or the backtick key.
- Added runtime resolution/presentation controls.
- Added internal-resolution scaling and anisotropic-filtering controls.
- Added optional enhanced presentation effects including:
  - bloom;
  - enhanced screen-space lighting;
  - ambient occlusion;
  - contact shadows;
  - sharpening.
- Graphics enhancements can be enabled or disabled independently from the optional PS3 remaster asset layer.

### Optional PS3 remaster asset layer

- Added support for user-supplied PS3 `PS3_FILES` assets.
- Added **extraction-less finalized PS3 PKG mounting** for the remaster layer:
  - the launcher can select a retail/finalized `.pkg` directly;
  - the package header and encrypted item table are parsed in place;
  - AES-128-CTR package data is decrypted by range on demand;
  - textures, TPK/material data, lighting files, meshes and ordinary package files are indexed without extracting the package;
  - EA BIG/VIV/C0FB containers stored inside the PKG are indexed through random-access decrypted reads;
  - `PS3_FILES` remains available as the directory-based fallback.
- PKG contents are never installed or materialized as a full extracted tree by the runtime.
- Added PS3 texture replacement paths.
- Added PS3 font replacement/rendering paths.
- Added PS3 TPK/material processing and experimental mesh/material porting paths.
- Added launcher controls to select the PS3 asset directory and enable/disable PS3 assets.
- Added an enhanced-graphics toggle independent of the base recompilation.
- The repository and release packages do **not** contain PS3 retail/remaster assets.

### Standalone recompilation launcher

- Added a dedicated **MOHFrontline-Recompiled** launcher instead of exposing the generic ModernGekko/Dolphin frontend to end users.
- Added Windows and Linux launcher builds.
- The launcher can:
  - select a supported GMFE69 disc image;
  - prepare/build the local game-derived native module;
  - launch the recompilation;
  - select a local `PS3_FILES` directory **or a finalized PS3 `.pkg` directly**;
  - enable/disable PS3 assets;
  - enable/disable enhanced graphics;
  - open the user directory;
  - open logs;
  - open the local AOT/disc cache.
- Added a standalone `MOHFrontline-DiscCache` helper backed by encounter/nod.

### Portable zero-development-dependency release

- Added self-contained Windows and Linux release bundles intended for use on a clean machine without installing the development toolchain.
- Portable bundles include:
  - CMake;
  - Ninja;
  - Zig;
  - Python;
  - DolRecomp;
  - encounter/nod support;
  - the prebuilt ModernGekko runtime;
  - the standalone launcher;
  - the disc-cache helper;
  - the minimal GXRuntime/StaticRecomp ABI sources required to compile the user-derived module.
- Windows launcher/runtime packaging uses the static MSVC runtime where configured to reduce external redistributable requirements.
- Added compact/minimal release payload staging so the end-user ZIP does not contain the complete Dolphin source tree or its very long SPIRV-Cross paths.
- Added SHA-256 sidecar files for release archives.
- Windows portable module generation pins **Zig 0.15.1** because Zig 0.16.0 regresses the Windows GNU shared-library link used by `gGMFE69_recomp.dll`; CI now performs a real ThinLTO DLL link smoke-test with the bundled toolchain.
- No original GameCube game data or PS3 assets are included in release archives.

### CI/CD

- Added cross-platform GitHub Actions CI for Linux and Windows.
- Added repository hygiene checks that reject tracked retail disc/remaster data.
- Added shell, PowerShell and Python validation.
- Added Linux and Windows ModernGekko core builds/tests.
- Added Linux and Windows production runtime builds with nod direct-disc support enabled.
- Added Linux and Windows standalone launcher/disc-cache builds.
- Added validation of the minimal portable end-user payload.
- Added a release workflow that builds:
  - `MOHFrontline-GC-RECOMP-vX.Y.Z-Windows-x86_64.zip`;
  - `MOHFrontline-GC-RECOMP-vX.Y.Z-Linux-x86_64.tar.gz`;
  - SHA-256 checksum files.
- Release publishing can be run manually and is also triggered by `v*` Git tags.
- GitHub Release notes are sourced directly from the matching version section in this changelog.

### Documentation

- Added a technical README describing the recompilation architecture, launcher, build process and runtime options.
- Added original GameCube and enhanced gameplay screenshots.
- Added documented PS3-assets + widescreen + custom-FOV presentation examples.
- Added executable-layout and GMFE69 optimization documentation.
- Added profiling and executable-inspection helper scripts.

### Known limitations

- **v0.0.1 is pre-alpha / experimental.**
- Only the USA GameCube release **GMFE69** is supported by the branded build.
- Gameplay works for development/testing, but compatibility is not considered final.
- Higher frame-rate targets are experimental because original game logic/timing was authored around console behavior.
- Long-session audio behavior and some low-address read paths remain areas of investigation.
- Some subsystems still rely on the ModernGekko/Dolphin compatibility chassis/fallback while native replacements continue to be developed.
- The user must provide a legally obtained copy of the original game.
- Optional PS3 enhancements require the user to provide their own legally obtained PS3 assets.
