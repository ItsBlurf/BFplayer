# Alpha.43 verification

Scope date: 2026-07-29

Build: `0.1.0-alpha.43`

## Exact artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,900,344 | `fad09e5b9ab19c0a1ed7ff6ab43f236caf20b77db14aa5b8d35269196d9f6a43` |
| `bfplayer-standalone.elf` | 17,920,112 | `bb233c4c831fb2105039197f33b549e57c35e44419b1329a7249235f34bd9e48` |
| `BFplayer-standalone.zip` | 17,465,953 | `2b28e2579e10960e91ffa4c0263e6e76cc4bb3807d778ae493981b283bf4b996` |
| `BFplayer-websrv.zip` | 17,452,129 | `88564d6142213662a5b837faf842b44ff75035d00f4e515ac65d9c0f81731e4c` |

The standalone verifier confirmed a FreeBSD x86-64 DYN payload, one embedded
gzip player image, Media-only tile metadata, and no websrv dependency. The
websrv verifier extracted all eight package entries and matched every file
against its manifest.

## Offline audit

- Current upstream PS5 payload SDK release: v0.41.
- Warning-clean PS5 Release build through the workspace SDK wrappers.
- All 25 host tests passed.
- GCC `-fanalyzer` core build and all 24 applicable tests passed.
- Exhaustive Cppcheck audit covered 80 application, vendored playback, and
  test files with no remaining diagnostics.
- GitHub CI includes a Linux GCC AddressSanitizer and UndefinedBehaviorSanitizer
  test job in addition to the normal host and DLNA integration tests.

## Live PS5 results

The exact standalone payload and the exact `BFplayer-websrv.zip` player were
each launched on PS5 and ran the same 13-gate playback harness.

| Check | Standalone | websrv |
| --- | ---: | ---: |
| 4K60 VP9/PQ source | 3840x2160 at 59.940 fps | 3840x2160 at 59.940 fps |
| Adaptive output | 1920x1080 | 1920x1080 |
| Median delivered rate | 59.940 fps | 59.940 fps |
| HDR conversion average | 1.494 ms | 1.388 ms |
| Peak RSS | 792,176 KiB | 791,664 KiB |
| WebM seek recovery | 647 ms | 323 ms |
| Audio-track recovery | 608 ms | 568 ms |
| Subtitle-track recovery | 312 ms | 615 ms |
| 1080p sample rate | 24.000 fps | 23.976 fps |

Both runs also passed completion, presentation latency, memory, pause-clock,
seek-accuracy, track-switch discontinuity, and clean-exit checks. The final
console state was restored to the standalone launcher, and a final launch/exit
smoke confirmed the `standalone-mmap` manifest and clean home return.

The supplied WebM also decodes without corruption warnings in current desktop
FFmpeg. Some dramatic color and distortion frames are present in the encoded
source itself; the player-side fixes address real-time delivery, HDR-to-SDR
conversion precision, and presentation backlog rather than removing authored
video effects.

## Remaining platform limit

The public PS5 homebrew SDK does not expose a supported hardware video decoder
API. BFplayer therefore keeps software decoding and uses adaptive output for
demanding 4K50/60 streams instead of relying on private, undocumented SCE
interfaces.
