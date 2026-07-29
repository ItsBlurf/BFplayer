# Alpha.38 offline verification

Scope date: 2026-07-29  
Version: `0.1.0-alpha.38`

## Claim boundary

The PS5 remained unavailable. No console connection, injection, launch, or
playback test was attempted. This record verifies the host-side DLNA protocol,
socket, HTTP, SOAP, DIDL, and resource-selection behavior plus the PS5 target
build. Live discovery and playback remain hardware-pending.

## Defects fixed

Some media servers publish several resources for one item, such as the
original file, a transcode, a thumbnail, or an unusable protocol. BFplayer
previously chose the first `http-get` entry before validating it. An invalid
first entry could therefore hide a valid later stream.

Alpha.38 validates every candidate before selection, rejects credentials and
unsupported protocols, prefers the audio or video MIME matching the item,
keeps server order for equal candidates, and recognizes generic items through
their protocol MIME. It also uses the server's advertised ContentDirectory
version, trims resource URL whitespace, and formats IPv6 HTTP authorities with
brackets.

## Verification results

| Gate | Result |
| --- | --- |
| Windows host CMake/Ninja build | Pass |
| Windows host CTest suite | 21 of 21 passed |
| Linux GitHub Actions build | Pass |
| Linux GitHub Actions CTest suite | 21 of 21 passed |
| Production DLNA client integration test | Pass in 1.50 seconds |
| PS5 Release build through workspace wrappers | Pass |
| Changed PS5-target static analysis | No diagnostics |
| Player SCE dependencies | All 10 present in the installed SDK |
| Standalone SCE dependencies | All 7 present in the installed SDK |
| Websrv package and naming checks | Pass |
| `git diff --check` | Pass |

GitHub Actions run:
<https://github.com/ItsBlurf/BFplayer/actions/runs/30465996816>

The expanded integration test verifies:

- ContentDirectory v2 SOAP action and XML namespace propagation;
- fallback from invalid credential and unsupported SMB resources;
- case-insensitive `http-get` recognition;
- whitespace-trimmed absolute resource URLs;
- media detection from protocol MIME when `upnp:class` is generic;
- preference for a resource whose MIME matches the item's media class;
- the existing paging, HTTP framing, limits, partial-failure, and cancellation
  behavior.

The host protocol suite separately verifies bracketed IPv6 authorities.

## Release artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,423,768 | `c765be40945cf995ae5388f9decd8e72ee2ce479c2149b892b95cd7bc80064d4` |
| `bfplayer-standalone.elf` | 17,756,272 | `e287db926bc646af2c37e0448861cc3b15c5a2b93b430bcccd2fff685ace583a` |
| `BFplayer-standalone.zip` | 17,312,362 | `5ec93be2de3faaf2c3e855f082f1035944fd6bbe351589193d5b84c1b9b06c9f` |
| `BFplayer-websrv.zip` | 17,289,803 | `22223978d9cde5b79a8dbae17a4790419f6b56c22161ba1b5f50025c7fb825e6` |

## Hardware work still required

A real PS5 and NAS or DLNA server are still required to verify multicast
discovery, controller navigation, stream handoff to FFmpeg, seeking, long-run
60 fps A/V sync, and measured peak memory.
