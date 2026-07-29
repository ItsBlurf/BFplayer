# Alpha.37 offline verification

Scope date: 2026-07-29  
Version: `0.1.0-alpha.37`

## Claim boundary

The PS5 remained unavailable. No console connection, injection, launch, or
playback test was attempted. This record adds executable integration evidence
for the production DLNA socket, HTTP, SOAP, XML, and paging path. PS5 network,
controller, playback, A/V sync, and memory behavior remain hardware-pending.

## Defect found and fixed

An absolute DLNA resource using an unsupported scheme, such as
`smb://nas/share/video.mkv`, was previously treated as a relative string by the
URL resolver. That could rebase it beneath the server's HTTP origin and make it
look like a supported HTTP resource.

Alpha.37 recognizes any syntactically valid absolute URI before relative
resolution. The existing stream allowlist then rejects unsupported schemes
without disguising them. Malformed container entries without an object ID are
also omitted because they cannot be browsed safely.

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
| Package inventory and ELF parity checks | Pass |
| `git diff --check` | Pass |

GitHub Actions run:
<https://github.com/ItsBlurf/BFplayer/actions/runs/30464866171>

The integration test executes `src/network/dlna_client.cpp` over real loopback
TCP sockets with TinyXML-2. It verifies:

- SOAPAction and request paging indices;
- chunked and Content-Length HTTP responses;
- namespace-aware SOAP and DIDL-Lite parsing;
- container, artwork, duration, size, resolution, audio, and video metadata;
- absolute and relative resource URL resolution;
- image, unsupported SMB, credential-bearing, and malformed-item filtering;
- hard result caps and truncation state;
- retained partial results after a later HTTP 500 response;
- cancellation before a delayed server response.

## Release artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,423,768 | `a25787f8c944f31696883865c2fcff8479b50fe3c82b6281119a335223d062cc` |
| `bfplayer-standalone.elf` | 17,756,272 | `b3e481dd931069a11c783f5849df3897faa0621e964dd3bf4e8bda018dc87aa8` |
| `BFplayer-standalone.zip` | 17,308,681 | `da69bff17bd8fddc85d52b5336454929634d7c2bf3031908672198f931f449ed` |
| `BFplayer-websrv.zip` | 17,288,351 | `9c968a5f90a42a0beac182e47ea7aa44614480519e887dade6cbe4549b5a7664` |

## Hardware work still required

The alpha.36 hardware ladder remains authoritative. Alpha.37 still needs a
real PS5 and NAS or DLNA server to verify multicast discovery, controller
navigation, return-to-folder behavior, stream handoff to FFmpeg, long-run
60 fps A/V sync, and measured `peak_rss_kib`.

