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
| Private repository/release | Repository now named `ItsBlurf/BFplayer`; prerelease `v0.1.0-alpha.9`, uploaded assets, and two successful GitHub Actions runs | Pass |

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
BFplayer/
BFplayer/assets/
BFplayer/assets/fonts/NotoSans-Regular.ttf
BFplayer/assets/fonts/OFL.txt
BFplayer/build-manifest.json
BFplayer/homebrew.js
BFplayer/bfplayer.elf
BFplayer/sce_sys/icon0.png
BFplayer/THIRD_PARTY_NOTICES.md
INSTALL-DIRECT-TILE.md
bfplayer-tile-installer.elf
```

`homebrew.js` remains in the application folder as a compatibility fallback.
The dashboard tile does not execute it.

## Artifact hashes

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 56,268,760 | `33ee6b9bd8de1eed709d056a9c817aef0dd6940a289de84620ce58c7d0897b20` |
| `bfplayer-tile-installer.elf` | 1,519,880 | `6e9ca682c57c9589c780743d3536f144006fb8b4d293babcac39e9073dcdb318` |
| `BFplayer-direct-tile.zip` | 25,896,387 | `f6421e2a05ff2733f88eecd40b194f3d7aa741982dcc681a77ddde4d7ae79bd9` |
| `assets/icon0.png` | 1,412,512 | `8c46982a3ee41c21ffce3b71fa52dcd50e61475a619bae58accee06603f5449b` |
| `docs/ui-preview.png` | 48,416 | `317ffd30bcbde70ac5d4ecbf4df0a57e32f9545926ee6b9fae2f7dfc11243cad` |

## Honest remaining gate

The build was not launched on PS5 because the owner explicitly prohibited
console testing for this revision. Dashboard registration, transition timing,
controller combinations, decoder performance, and final pixels/audio remain
runtime gates. No offline test can prove those hardware-specific behaviors.
