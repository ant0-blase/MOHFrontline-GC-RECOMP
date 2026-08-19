# Portable platform roadmap (v9)

The v9 patch separates the **game/static-recomp core** from the platform shell. The same MOHF
camera, FOV, timing, HUD and logical input layer is intended to be shared everywhere.

## Status

| Platform | v9 status | Input path | Module format |
|---|---|---|---|
| Linux Wayland | runtime target, tested by the main build | relative-pointer + pointer constraints | `.so` |
| Linux X11 | desktop fallback added in v9 | X11 grab + relative warp fallback | `.so` |
| Windows x64 | native build script + Raw Input backend | `WM_INPUT`, keyboard, mouse buttons/wheel | `.dll` |
| Android arm64 | **foundation** | C ABI touch/action bridge | `.so` |
| iOS arm64 | **foundation** | C ABI touch/action bridge | static `.a` |

Android and iOS entries deliberately mean **core/build foundation**, not a finished mobile app.
They still need a native surface/presentation shell, lifecycle glue, touch HUD and packaging/signing.

## Shared mobile bridge

`moderngekko/moh_mobile_bridge.h` exports a tiny ABI:

- `moh_mobile_set_viewport(width, height)`
- `moh_mobile_touch_look(dx, dy)`
- `moh_mobile_touch_action(action, down)`
- settings/debug toggles

The mobile frontend therefore does not need to know GameCube button numbers or MOHF camera offsets.
It feeds logical actions into the same layer used by desktop mouse/keyboard.

## Android

Run on Linux/macOS with an NDK:

```bash
ANDROID_NDK_HOME=/path/to/android-ndk ./scripts/build-android.sh
```

This builds the ARM64 mobile bridge and an ARM64 `gGMFE69_recomp.so`. A future Android app should
provide an `ANativeWindow`, Vulkan surface, audio device, lifecycle and on-screen controls.

## iOS

Run on macOS with Xcode:

```bash
./scripts/build-ios.sh
```

iOS uses a **static** recomp module foundation because arbitrary runtime-loaded unsigned dylibs are
not a viable app architecture. The next iOS milestone is to wire the static recomp descriptor into
an Objective-C++/Swift shell and use Metal directly or the existing Vulkan renderer through
MoltenVK where appropriate.

## Windows

From a Developer PowerShell with CMake, Ninja, Python and Clang available:

```powershell
.\scripts\build-windows.ps1 -Iso D:\Games\MOHFrontline.iso
.\scripts\run-windows.ps1
```

The Windows backend uses Raw Input for frame-rate-independent mouse motion. The in-game PC settings
and gamepad coexistence are shared with Linux.

## Packaging rule

No retail game data belongs in release archives. Distribute runtime/module/build tooling only and
require the user to provide their own GMFE69 disc image or extracted files.
