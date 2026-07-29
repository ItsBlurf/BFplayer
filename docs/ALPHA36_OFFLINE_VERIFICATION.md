# Alpha.36 offline verification

Scope date: 2026-07-29  
Version: `0.1.0-alpha.36`

## Claim boundary

The PS5 was down for this work. No console probe, upload, injection, launch, or
playback test was attempted. This record proves the host-tested logic, PS5
target build, dependency boundary, and package identity. It does not claim
that A/V sync, memory use, DLNA interoperability, controller behavior, or
network playback has been observed on PS5 hardware.

## Implemented scope

- Audible SDL audio is the primary Kitchensink clock when audio is present.
- Local and network playback have separate bounded packet/frame policies.
- Network input has reconnect handling plus queue and peak-RSS diagnostics.
- The library can discover and browse DLNA/UPnP MediaServer devices.
- Users can open direct supported network media URLs.
- DLNA HTTP, SSDP, XML, paging, cancellation, response, and result bounds are
  explicit.
- The existing SDL2/TTF UI remains in place after reviewing RmlUi and
  `ps5-payload-dev/dlnaplay` at commit
  `2f7cfbf840e47b064ad628257ef59e558d30736d`.

## Verification results

| Gate | Result |
| --- | --- |
| Host CMake/Ninja build | Pass |
| Host CTest suite | 21 of 21 passed |
| New DLNA protocol tests | Pass |
| New audible-audio clock tests | Pass |
| PS5 Release build through workspace wrappers | Pass |
| Changed C/C++ target static analysis | Pass after removing one dead store |
| Player ELF | FreeBSD x86-64 DYN/PIE, entry `0x40` |
| Standalone ELF | FreeBSD x86-64 DYN/PIE, entry `0x40` |
| Player SCE dependencies | All 10 present in the installed SDK |
| Standalone SCE dependencies | All 7 present in the installed SDK |
| Standalone ZIP inventory and embedded ELF parity | Pass |
| websrv ZIP inventory and `eboot.elf` parity | Pass |
| `git diff --check` | Pass |

The target static-analysis set covered:

- `src/core/dlna_protocol.cpp`
- `src/network/dlna_client.cpp`
- `src/ui/library_ui.cpp`
- `src/main.cpp`
- `src/playback/kitchensink_audio_clock.c`
- `vendor/SDL_kitchensink/src/kitplayer.c`
- `vendor/SDL_kitchensink/src/internal/audio/kitaudio.c`

## Release artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,407,384 | `2cf49173de25c4b5992bdd322ba0af78fb42c829ebb473768f4e20fa9c51de42` |
| `bfplayer-standalone.elf` | 17,756,272 | `e41d704175fd29ca5db0f14ed2809294397898a9199c64fd3fcffd93088985e4` |
| `BFplayer-standalone.zip` | 17,308,811 | `fc06b35b76f957ab462bc50e1a9f8f4b43dad42a48e3a2f0e530a1b5ffc32f43` |
| `BFplayer-websrv.zip` | 17,287,900 | `6c4bbc41b6dfaa5f929c68c9fe4a531336e584f64873bc00ca0fc50638a0ad93` |

The `BFplayer-websrv.zip` `BFplayer/eboot.elf` hash exactly matches
`bfplayer.elf`. The standalone ZIP payload hash exactly matches the standalone
ELF above.

## Hardware work still required

Follow `HARDWARE_TEST_LADDER.md` when the console is available. Alpha.36
specifically needs:

1. a long 60 fps LAN stream sync test;
2. repeated seeking and audio/subtitle switching;
3. queue, `audio_queue_ms`, delivered-frame-rate, and `peak_rss_kib`
   measurement;
4. DLNA discovery, nested browsing, refresh, cancellation, failure, playback,
   and return-to-folder tests against at least one real NAS or media server.

