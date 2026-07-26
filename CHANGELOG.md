# Changelog

## 0.1.0-alpha.13 - single-tile test build

### Dashboard registration

- Consolidated the visible Media tile and BigApp runtime host onto the single
  `PSMC00001` title ID.
- Changed the repaired `/system_ex/app/PSMC00001` host metadata to the
  Prospero Native Media App category.
- Removed the separate `PSMR00001` native-Game registration that caused a
  duplicate PS5 Media Center entry in Games.
- Added a startup migration that uninstalls the legacy `PSMR00001`
  registration and removes its old `/user/app` metadata before registering the
  single Media entry.

### Add Media reliability

- Fixed the `/` source browser so PS5 mount points such as `/data`, `/user`,
  and attached storage remain visible and can be entered explicitly.
- Kept the picker descriptor-bound and symlink-safe, with PS5 removable-drive
  `lstat` fallback, bounded listings, fatal-I/O aborts, and visible errors.
- Added a combined traversal budget to bulk library import so many child
  folders cannot bypass the per-folder limits or silently return partial
  results.
- Roll back in-memory source changes when database persistence fails.

## 0.1.0-alpha.12 - manual-library test build

### Manual library management

- Removed startup storage discovery and the Rescan action. Only media sources
  explicitly selected by the user are indexed and retained in the library.
- Add Media now opens at the PS5 filesystem root (`/`).
- Added a two-press Triangle confirmation to remove the selected movie or TV
  show source and its cached rows from the library.
- Added an explicit bulk import action for a chosen folder: loose video files
  become Movies, while immediate child folders containing video recursively
  become TV Shows.
- Kept recursive source traversal descriptor-bound, symlink-safe,
  same-filesystem-only, bounded, cancellable, and fatal-I/O aware.

### Artwork and launch handoff

- Added bounded background FFmpeg extraction of an interior video frame when
  no valid local JPEG/PNG artwork is available, including alternate positions
  to avoid mostly black frames.
- Removed the old logo from the empty artwork panel.
- Replaced the white loopback launch response with a dark three-second
  countdown page that attempts to close or return automatically.
- Preserved `PSMC00001` as an English-only Media-category dashboard tile.

## 0.1.0-alpha.11 — independent runtime-host test build

### HBL-independent startup

- Replaced HBL's shared `FAKE00000` dependency with the project-owned
  `PSMR00001` BigApp host.
- Added automatic AppInst registration plus `/system_ex` host creation and
  repair before the dashboard tile is published.
- Rewrites missing, zero-length, partial, symlinked, or invalid host artifacts
  safely instead of treating any existing path as usable.
- Recreates all required `/data/homebrew/PS5-MediaCenter` runtime assets; the
  folder is not required to remain on the console.
- Uses the Media Center icon and title for the active BigApp host so the PS
  switcher can identify the running app correctly.

### Payload size and integrity

- Strips the player ELF and embeds it as gzip instead of embedding the raw
  link output.
- Expands the player in memory on demand, validates the exact decompressed size
  and ELF64 header, and logs compression/launch diagnostics.

### Interface

- Replaced the procedural in-app play/blade mark with the exact dashboard-tile
  artwork.
- Replaced footer button names with rendered PlayStation-style Cross, Circle,
  Square, Triangle, D-pad, touchpad, stick, and Options prompts.

## 0.1.0-alpha.10 — standalone test build

### Self-contained dashboard launch

- Replaced the websrv/port-8080 launch path with one resident
  `ps5-media-center-standalone.elf`.
- Embedded the complete player ELF, Noto Sans runtime font, icon, tile data,
  AppInst setup, and minimal BigApp/ELF-loader core into that single payload.
- Added a loopback-only listener on `127.0.0.1:9040` with exactly one
  `GET /launch` route.
- Removed the separate tile-installer ELF, `homebrew.js`, websrv package, HBL
  catalog path, and file-picker dependency.
- Added standalone launcher/player logs and an embedded runtime manifest.
- Added GPL-3.0-or-later source attribution and license text for the retained
  upstream BigApp loader core.

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
