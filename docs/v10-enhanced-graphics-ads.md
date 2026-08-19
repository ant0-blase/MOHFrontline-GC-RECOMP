# v10 - Enhanced Graphics + FPS ADS

This optional layer keeps the original GameCube renderer/gameplay available at all times.

## Enhanced Graphics

`Enhanced Graphics` selects the built-in `MOHFrontlineEnhanced` Dolphin post-process. Turning it off restores the previously selected post shader. It provides bloom, filmic tone mapping, local-light enhancement, luminance-based screen-space AO/contact-shadow enhancement, sharpening, cinematic radial DOF, vignette and film grain.

The AO/contact-shadow and DOF passes are screen-space presentation effects; they do **not** replace Frontline's original lights with new PBR lights or shadow maps. This keeps the first implementation portable across Vulkan/D3D/Metal and fully reversible.

## FPS ADS

FPS ADS leaves the original Frontline aim mechanics active while adding a modern presentation layer: smooth world/viewmodel FOV transition, direct viewmodel centering in `CPlayerWeaponObject::UpdateWeaponTransforms`, ADS sensitivity, optional crosshair hiding and optional ADS DOF. The viewmodel hook edits only the temporary translation vector on the guest stack, so the original animation data is never permanently modified.

Because weapon animations differ, X/Y/Z sight target sliders are exposed live for calibration.
