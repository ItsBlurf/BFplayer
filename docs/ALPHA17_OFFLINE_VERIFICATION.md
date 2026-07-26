# Alpha.17 offline verification

Version: `0.1.0-alpha.17`

This pass verifies the resume, subtitle-timing, playback-setting, and nested
season changes without claiming unobserved PS5 hardware behavior.

## Results

| Area | Evidence | Result |
| --- | --- | --- |
| Host regression suite | Release build; 13/13 CTest tests pass | Pass |
| Season grouping | Nested, direct-child, sibling-boundary, and separator-normalization cases | Pass |
| Resume order | Saved seek is queued while stopped and applied only after `Kit_PlayerPlay` | Pass by source/target analysis |
| Resume final save | Pre-cleanup position and duration snapshot used for the final database record | Pass by source/target analysis |
| Subtitle timing | External renderer and embedded SDL_kitchensink timestamp receive the same signed delay | Pass by source/target analysis |
| OSD setting | Renamed to Pop-up message duration with an in-app explanation | Pass |
| Filesystem audit | Analyzer-found stale `errno` read removed; `fdopendir` error captured before close | Pass |
| Host static analysis | GCC `-fanalyzer`; no project-code analyzer defect | Pass |
| PS5 static analysis | Changed player, library UI, and Kitchensink adapter analyzed through SDK wrappers | Pass |
| PS5 compile/link | Release build through required SDK wrappers and PacBrew v0.37 | Pass |
| Standalone structure | FreeBSD x86-64 DYN ELF; embedded-player verifier passes | Pass |
| Local hygiene | No console IP, credential/private-key pattern, or unsafe C string call in active source | Pass |

The GCC analyzer again reports its known path-sensitive buffer warning inside
the generated SQLite 3.53.3 amalgamation. No authored source warning remains,
and the SQLite regression suite passes.

## Artifacts

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `ps5-media-center-standalone.elf` | 17,640,688 | `ef986e2d18ea6e9bde0a654631b9b1a9059761844cfe43a85508bd90cfff3bb3` |
| `PS5-MediaCenter-standalone.zip` | 17,188,994 | `adfa265d6e5dfb840cf53fd6fb035660b57956e2765d56412f0b16e6cd4c5a10` |
| Embedded stripped player | 36,177,808 | `2db9d98483f0d7979ba03c8c374b29abbc8994b373740828adbdf559cb4d121b` |

The remaining gate is a PS5 controller/media acceptance run covering one
embedded text-subtitle file, one sidecar subtitle, resume after returning to
the library, and a Show/Season/Episode folder tree.
