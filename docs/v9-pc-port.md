# MOHFrontline v9 PC / Portable Edition foundation

v9 keeps the original GMFE69 engine and static-recompiled code path, while moving host-facing
concerns into a native platform layer. It is intentionally a preservation-oriented port rather
than a rewrite.

## What v9 adds

### Input

- Native, frame-rate-independent mouse look using the live `CPlayerObject*` from
  `CPlayerObject::BeginUpdate`.
- Separate base/X/Y/ADS mouse sensitivity, optional smoothing/acceleration, X/Y inversion.
- Hold/toggle aim and hold/toggle crouch.
- Rebindable keyboard actions, AZERTY/QWERTY aliases, mouse buttons and wheel weapon switching.
- Physical gamepad coexistence plus host-side stick sensitivity/deadzone.
- Stock GameCube menus remain navigable with mouse movement + LMB/RMB; the PC settings overlay has
  true cursor hit-testing.

### Video / UI

- World Hor+ FOV and independent weapon FOV.
- 4:3, 16:10, 16:9, 21:9, 32:9 and arbitrary aspect ratios.
- Independent 2D safe-area transform, HUD scale and safe-width controls so UI is not stretched.
- Internal resolution, anisotropic filtering, MSAA, texture filtering, true-color and copy-filter
  controls exposed in-game.

### Frame pacing / audio

- Gameplay-only VI frequency override; frontend/loading timing stays native.
- Simulation delta is based on real elapsed time rather than assuming the requested FPS was hit.
- v9 adaptive VI uses an EMA, hysteresis and slow recovery profiles instead of rapidly oscillating.
- Audio buffer, gap filling and pitch preservation are exposed in-game.

### In-game PC settings

`Ctrl+F10` or the grave key opens the settings overlay. `Ctrl+F8` toggles the diagnostics overlay.
Settings persist in `user/moh_pc_settings.ini` and most changes are live.

### Portability

- Linux: Wayland relative-pointer path + X11 fallback.
- Windows x64: native Win32 Raw Input path and PowerShell build/run/package scripts.
- Android arm64: cross-buildable static-recomp module + C mobile input bridge foundation.
- iOS arm64: static module/mobile bridge foundation for an eventual signed Xcode shell.

## Deliberate v9 limitations

Android/iOS are **foundations, not finished applications**. A production mobile app still needs
surface/presentation lifecycle integration, audio output, a touch HUD, suspend/resume handling,
packaging and signing. iOS intentionally uses a static recomp module architecture.

The original frontend does not expose generic UI hit-test rectangles, so stock menus use mouse-to-
navigation translation rather than pretending every original button is a native host widget.

Keyboard/controller glyph replacement requires game-asset/layout work and is left as a later
presentation milestone rather than hardcoding incorrect prompts in v9.

## Release rule

Never package retail game data. Release archives contain only runtime/module/tooling and an empty
`extracted/` placeholder. Users must provide their own GMFE69 disc/extracted data.
