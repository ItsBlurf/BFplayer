# Alpha.39 offline verification

Scope date: 2026-07-29  
Version: `0.1.0-alpha.39`

## Claim boundary

The PS5 remained unavailable. No console connection, injection, launch,
playback, or live memory measurement was attempted. This record verifies the
packet-memory fix, host behavior, PS5 target build, package contents, and
linked dependencies. Runtime memory use and playback quality remain
hardware-pending.

## Defect fixed

BFplayer deliberately uses larger compressed input queues than upstream
SDL_kitchensink to absorb removable-storage and network jitter. Those queues
were bounded by packet count but not by packet bytes. A small number of large
compressed packets could therefore consume much more memory than intended.

Alpha.39 adds separate payload budgets of 96 MiB for video, 16 MiB for audio,
and 8 MiB for subtitles. Packet side data is included in the accounting. One
oversized packet may enter an empty queue to preserve forward progress, while
seek and shutdown wake blocked writers safely. Flush, read, and teardown paths
reset or subtract the accounting without underflow.

The decoded-frame queues remain deliberately small. This change bounds the
compressed input side without claiming that FFmpeg decoder allocations,
converted textures, or every source format fit inside the same limits.

## Verification results

| Gate | Result |
| --- | --- |
| Windows host CMake/Ninja build | Pass |
| Windows host CTest suite | 22 of 22 passed |
| Packet-budget regression suite | Pass |
| Linux GitHub Actions build | Pass |
| Linux GitHub Actions CTest suite | 22 of 22 passed |
| Production DLNA client integration test | Pass in 1.50 seconds |
| PS5 Release build through workspace wrappers | Pass |
| Changed PS5-target static analysis | No diagnostics |
| Player SCE dependencies | All 10 present in the installed SDK |
| Standalone SCE dependencies | All 7 present in the installed SDK |
| Websrv package and naming checks | Pass |
| `git diff --check` | Pass |

Source-change GitHub Actions run:
<https://github.com/ItsBlurf/BFplayer/actions/runs/30472425302>

The packet-budget tests cover unlimited queues, exact-limit admission,
over-budget rejection, one oversized packet in an empty queue, overflow
saturation, and underflow prevention.

## Release artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,423,768 | `6663709b274d3db43665a0ae88669bf88a16353bf8479d55268baf7c0a5cca48` |
| `bfplayer-standalone.elf` | 17,756,272 | `9b3b17ec0b267c3cb3d9c2553ae291e4a610fcdefc42c91cfe07f4b84e791327` |
| `BFplayer-standalone.zip` | 17,315,278 | `c29c271d4e48ecd36041d33228297a743f8513938744c54e7de37cc1a9895a05` |
| `BFplayer-websrv.zip` | 17,289,446 | `daab41d15dfa5e64f2c1a4028cb171259d8a8774eaffd42360169a2fa2536529` |

## Repository audit

The final local tree was clean before this verification record. The repository
has one unrelated open feature request for richer graphical gamepad prompts;
the current footer already renders vector face-button, touchpad, and Options
glyphs, so a broader UI redesign was not mixed into this stability release.
Dependabot and secret scanning are disabled for the repository, while the
available local token-pattern scan found no credential material.

## Hardware work still required

A real PS5 and representative local, USB, NAS, and DLNA sources are still
required to measure peak RSS, validate long-run 60 fps A/V sync, and confirm
seek, track switching, controller navigation, reinjection, and shutdown
behavior. Offline evidence cannot guarantee those hardware-specific results.
