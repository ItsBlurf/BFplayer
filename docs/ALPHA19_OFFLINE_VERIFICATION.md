# Alpha.19 offline verification

Version: `0.1.0-alpha.19`

This pass verifies the library-sort and idempotent-reinjection changes without
claiming that takeover timing or controller presentation has been observed on
PS5 hardware.

## Results

| Area | Evidence | Result |
| --- | --- | --- |
| Host regression suite | Release build; 13/13 CTest tests pass | Pass |
| TV-show name sort | Collapsed show rows receive an explicit display-name ordering pass | Pass by source/target analysis |
| Sort selection | Selected series identity is restored after the representative episode changes | Pass by source/target analysis |
| Sort shortcut | Main library footer renders `R3 Sort: <mode>` | Pass |
| Singleton update | Exclusive `flock` is held for the resident launcher's lifetime | Pass by source/target analysis |
| Graceful takeover | Existing launcher is stopped before updates; replacement port 9040 binds only after completion | Pass by structural test |
| Idempotent files | Managed regular files are byte-compared and only changed files use atomic replacement | Pass by source/target analysis |
| PS5 static analysis | Changed launcher and library UI pass through the required SDK wrappers | Pass |
| PS5 compile/link | Release build through SDK wrappers and PacBrew v0.37 | Pass |
| Standalone structure | FreeBSD x86-64 DYN ELF; embedded-player verifier passes | Pass |
| Local hygiene | Diff and active-source secret/IP/unsafe-string scans | Pass |

The standalone ELF imports `flock` for the lifetime lock. The service remains
loopback-only and the takeover still uses the exact `GET /shutdown` route.
Runtime updates preserve `/data/PS5-MediaCenter/library.db`; no library,
settings, playback-history, or source-removal path was added to reinjection.

## Artifacts

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `ps5-media-center-standalone.elf` | 17,641,096 | `76c5d39b0164270b95e11217dc462f99ef6f08340892da8e75e8cbf53073720b` |
| `PS5-MediaCenter-standalone.zip` | 17,197,366 | `6abf8021841d95cf167ac807d410a41d780963117b8e21ae74db6266cda30093` |
| Embedded stripped player | 36,194,192 | `f0aa30287551112bcea131a578354a5ff4573fa64847f288b3aadfae2d749ee5` |

The remaining hardware gate is to reinject alpha.19 over alpha.18 twice,
confirm one ready notification and one Media tile, and verify that R3 changes
the displayed sort while keeping the same selected TV show.
