# PC Edition layer

The GMFE69 recompilation includes an optional PC-facing layer on top of the original game logic.
It does not replace the GameCube input or frontend: a physical gamepad remains usable and the
original UI is still the authoritative menu system.

## Keyboard and mouse

Default bindings:

| PC input | Game action |
|---|---|
| W/Z, A/Q, S, D | Move |
| Mouse | Look |
| Left mouse | Fire |
| Right mouse | Aim |
| E | Action / use |
| R | Reload |
| F / middle mouse | Melee |
| Space | Jump |
| C / Ctrl | Crouch |
| Mouse wheel / 1 / 2 | Weapon cycle |
| Tab | Call HQ / objectives |
| Esc | Pause / back |

Wayland uses `zwp_relative_pointer_v1` plus pointer constraints for true relative mouse input.
The cursor is captured only during gameplay and is released automatically while the PC settings
overlay is open.

The stock GameCube frontend has no native mouse hit-testing. The PC layer therefore maps mouse
movement to menu navigation and left/right click to A/B. The new PC settings overlay itself uses
native ImGui hit-testing and is fully clickable.

## PC settings overlay

Press **Ctrl+F10** or **`** to open the live settings overlay.

Available controls include:

- FPS target: original, 60, 90, 120, 144, 165, 240 or unlimited
- adaptive FPS/audio protection
- world FOV and independent weapon/viewmodel FOV
- original, auto, 16:10, 16:9, 21:9, 32:9 or custom aspect ratio
- aspect-correct HUD/menu safe area
- 1x-8x internal resolution
- VSync
- keyboard rebinding
- mouse sensitivity and invert-Y
- simultaneous physical gamepad enable/disable
- volume, audio buffer, gap filling and pitch preservation

Settings are stored in `user/moh_pc_settings.ini`.

## Aspect-ratio model

3D uses Hor+ projection. 2D uses the original 4:3 authored canvas with a centered contain transform.
This prevents text, icons and menus from becoming horizontally stretched on 16:10, 16:9 and
ultrawide displays. It also handles arbitrary landscape, square and portrait aspect ratios.

`--hud stretch` restores the original stretched 2D presentation for comparison.

## Audio-safe high FPS

The requested FPS is a ceiling, not a promise to slow the emulator down until it reaches that
number. When the host cannot sustain the requested VI overclock, the PC layer reduces the effective
VI rate toward a full-speed value. The game simulation is already driven by real elapsed time, so
this changes rendered FPS without changing game speed.

The layer also enables audio gap filling and pitch preservation by default and exposes the audio
buffer in the live settings menu. This is intended to keep audio stable when the selected frame-rate
ceiling is above what the current machine can sustain.
