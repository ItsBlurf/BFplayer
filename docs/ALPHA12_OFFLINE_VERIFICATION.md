# Alpha.12 offline verification

Version: `0.1.0-alpha.12`

This test build changes the media library to explicit user-managed sources,
adds bulk import/removal and generated video previews, and improves the
loopback browser handoff. No console deployment was performed during this
verification, so new runtime behavior still requires the hardware ladder.

## Results

| Check | Evidence | Result |
| --- | --- | --- |
| PS5 target build | SDK wrappers produced x86-64 FreeBSD-ABI ET_DYN player and standalone ELFs | Pass |
| Host tests | All 12 CTest targets passed, including bulk import, manual-source filtering, and root removal/cascade coverage | Pass |
| Manual library | Default storage roots and startup scanning were removed; normal launch loads only configured-source cache | Pass |
| Bulk import | Loose direct videos become Movies; immediate child folders with recursively discovered video become TV Shows; `/` import is rejected | Pass |
| Traversal safety | Browser/import use descriptor-bound, no-follow, same-device traversal with limits and fatal-I/O aborts | Pass |
| Remove source | Two-press UI confirmation persists source removal and transactionally deletes its SQLite root/media rows | Pass |
| Preview fallback | Target build links bounded/cancellable FFmpeg frame extraction at 50%, 35%, or 65%, with 30/10/2-second fallbacks for unknown duration | Pass |
| Tile category | `PSMC00001` remains English-only with `applicationCategoryType: 65536` (Media); `PSMR00001` remains the required BigApp host | Pass |
| Launch handoff | Loopback route serves dark HTML with a three-second countdown and close/history fallback; other routes remain 404 | Pass |
| Standalone embedding | Gzip expands byte-for-byte to the stripped player; embedded assets occur exactly once | Pass |
| SCE imports | All 10 player and 7 standalone dependencies are present in the installed payload SDK | Pass |
| Package | Exact five-file archive inventory and manifest/hash verification passed | Pass |

## Final artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,128,656 | `e229101d2fd9a154221550ce0b8d6c2a684e0a0d226bea1aa6ef57cd942bf307` |
| `bfplayer.elf.gz` (embedded intermediate) | 15,439,867 | `4c5e0cbb16bc82723d66626365c3b08872c7a2058e4ac334c9d32bd60a03777a` |
| `bfplayer-standalone.elf` | 17,607,752 | `ebf075116ef964db47380e5d0c32d7b71dd4f845922e94b3195e9b0f5a2d71a7` |
| `BFplayer-standalone.zip` | 17,160,843 | `c2bb06fc4f091e1c9f0a882da841f920bd7358c7f13561c8f492b9bc98811dc0` |

## Hardware gate

On PS5, verify the Media dashboard category, root-first browser, lack of
automatic scanning, explicit single/bulk add, two-press removal, generated
interior video previews, and browser-countdown return. Follow
`HARDWARE_TEST_LADDER.md` and collect launcher/player logs before another run.
