# Changelog

## 0.1.0-alpha.9 — offline test build

### Display controls

- Added all VLC Desktop display-aspect choices: Default, 1:1, 4:3, 16:9,
  16:10, 2.21:1, 2.35:1, 2.39:1, and 5:4.
- Added all VLC Desktop crop choices: Default, 16:10, 16:9, 1.85:1, 2.21:1,
  2.35:1, 2.39:1, 5:3, 4:3, 5:4, and 1:1.
- Kept source crop and display-aspect override independent, matching VLC
  semantics.
- Added Best fit, Fill screen (crop), and Fullscreen stretch resize modes.
- Added independent persistent SQLite settings and diagnostic fields for scale,
  aspect, and crop.
- Added exhaustive host geometry tests, including anamorphic pixel-aspect
  correction.

### Dashboard launch

- Added a `PSMC00001` Media-category dashboard tile.
- Added a separate `ps5mc-tile-installer.elf` so AppInst/kernel-system
  privileges do not enter the player ELF.
- The tile calls websrv's `/hbldr` route with the exact Media Center ELF and
  working directory, bypassing the catalog, `homebrew.js`, and file picker.
- Added a primary direct-tile bundle, exact SHA-256 manifests, and install
  documentation.
- Kept the unsafe compatibility-FPKG/OpenOrbis experiments out of the package.

### Visual redesign

- Replaced the glossy generic icon with a custom BFpilot-inspired pixel-art
  blade piercing a gold play symbol.
- Rebuilt the library around a compact branded header, status pill, bordered
  content panels, blue/gold selection treatment, real empty-library guidance,
  improved artwork empty state, and a smaller control rail.
- Added a deterministic 1920×1080 UI preview renderer and checked-in preview.

### Verification scope

- Built with the PS5 SDK wrappers as a FreeBSD-ABI x86-64 DYN payload.
- Passed all host tests and PS5-target compile/link checks.
- Passed SDK dependency checks for both player and tile installer.
- No console connection, deployment, or PS5 runtime test was performed for this
  build, per owner instruction.
