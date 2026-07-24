# 0.1.0-alpha.9 offline verification

Date: 2026-07-24
Scope: host and PS5-target checks only; the PS5 was not contacted.

## Requirements checked

| Requirement | Evidence | Result |
| --- | --- | --- |
| Complete VLC Desktop aspect list | 9 enum/key/name entries; exhaustive ratio loop in `video_layout_tests` | Pass |
| Complete VLC Desktop crop list | 11 enum/key/name entries; exhaustive source/output-ratio loop | Pass |
| Exact 4:3, 16:9, ultrawide, anamorphic behavior | Deterministic rectangle assertions, including 720×480 anamorphic crop | Pass |
| Fullscreen behavior | Fullscreen stretch maps the selected source rectangle to 1920×1080 | Pass |
| Persistent independent controls | SQLite keys `video_scale_mode`, `video_aspect_mode`, and `video_crop_mode` | Pass |
| Direct dashboard launch package | `PSMC00001` param, `/hbldr` URL parser test, separate installer, exact ZIP listing | Offline pass |
| No visible catalog/picker in tile path | Deeplink targets `/hbldr` directly; no `homebrew.js` request in the URL | Source pass |
| Player privilege boundary | Player has no `libSceAppInstUtil` or `libkernel_sys`; installer has both | Pass |
| UI redesign | SDL source compiled; deterministic 1920×1080 preview inspected | Offline pass |
| Pixel-art icon | PNG integrity/dimensions inspected; package hash matches source | Pass |
| Private repository/release | Verified after GitHub publication | Pending at this document revision |

## Host tests

The MinGW/Ninja host build completed and CTest reported:

```text
11/11 tests passed
```

Coverage includes library traversal/view/source behavior, artwork bounds,
playlists, source URL policy, safe local reads, SQLite persistence/migration,
homebrew compatibility launcher validation, direct-tile validation, and video
layout geometry.

## PS5-target build

The build loaded `ps5dev-env.ps1` and used the project SDK wrappers. Both files
were identified as:

```text
ELF64
OS/ABI: UNIX - FreeBSD
Machine: x86-64
Type: DYN (position-independent executable)
Entry point: 0x40
```

SDK dependency checking reported every `NEEDED` library available.

Player imports include the PS5 web kernel/libc, SystemService/UserService, Pad,
AudioOut, VideoOut, Keyboard, IME, and Net stubs. They do not include
`libSceAppInstUtil` or `libkernel_sys`.

The installer imports `libkernel_sys`, `libSceAppInstUtil`,
SystemService/UserService, libc, and wrapper-provided web/net stubs.

## Package listing

```text
PS5-MediaCenter/
PS5-MediaCenter/assets/
PS5-MediaCenter/assets/fonts/NotoSans-Regular.ttf
PS5-MediaCenter/assets/fonts/OFL.txt
PS5-MediaCenter/build-manifest.json
PS5-MediaCenter/homebrew.js
PS5-MediaCenter/ps5-media-center.elf
PS5-MediaCenter/sce_sys/icon0.png
PS5-MediaCenter/THIRD_PARTY_NOTICES.md
INSTALL-DIRECT-TILE.md
ps5mc-tile-installer.elf
```

`homebrew.js` remains in the application folder as a compatibility fallback.
The dashboard tile does not execute it.

## Artifact hashes

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `ps5-media-center.elf` | 56,268,760 | `af32bbb022a38a349406562b5d9ef8457139098ab7a05228e488ff3581fd4059` |
| `ps5mc-tile-installer.elf` | 1,519,880 | `af9754fb1a12401db25c79e99c22f4fd221c8b74e6148dae1b2ddcff94912820` |
| `PS5-MediaCenter-direct-tile.zip` | 25,896,583 | `f9f929828cd12e5c80f263c231b3b5e813015bcddcb82b2d4e77ada3ccd87f33` |
| `assets/icon0.png` | 1,412,512 | `8c46982a3ee41c21ffce3b71fa52dcd50e61475a619bae58accee06603f5449b` |
| `docs/ui-preview.png` | 48,416 | `317ffd30bcbde70ac5d4ecbf4df0a57e32f9545926ee6b9fae2f7dfc11243cad` |

## Honest remaining gate

The build was not launched on PS5 because the owner explicitly prohibited
console testing for this revision. Dashboard registration, transition timing,
controller combinations, decoder performance, and final pixels/audio remain
runtime gates. No offline test can prove those hardware-specific behaviors.
