# Alpha.16 offline verification

Version: `0.1.0-alpha.16`

This pass intentionally stops before console deployment. It verifies the
complete source, host-test, PS5-build, standalone-launcher, package, and
repository state that can be checked without treating an unobserved hardware
interaction as proven.

## Results

| Area | Evidence | Result |
| --- | --- | --- |
| Host regression suite | Release build; 13/13 CTest tests pass | Pass |
| Atomic settings | SQLite batch-commit and forced mid-batch rollback test | Pass |
| Bulk import | Movie/TV classification, progress, cancellation, root rejection, fatal-I/O precedence | Pass |
| Add Media input | Controller and dedicated keyboard routing; first-run Cross is not gated by scanner state | Pass |
| Filesystem policy | `lstat`, no-follow descriptor checks, symlink rejection, device boundary, fatal-I/O abort, traversal budgets | Pass |
| Static analysis | GCC `-fanalyzer` build completed; project code produced no analyzer defect | Pass |
| PS5 compile/link | SDK wrappers, Release flags, PacBrew v0.37, no compiler/linker failure | Pass |
| Standalone structure | 64-bit little-endian FreeBSD x86-64 DYN ELF; standalone verifier passes | Pass |
| Dashboard metadata | `PSMC00001`, English only, Media category `65536`, loopback `127.0.0.1:9040/launch` | Pass |
| Runtime independence | No websrv/HBL/port-8080 dependency; embedded player and runtime assets present | Pass |
| Visual QA | Empty-library and Add Media 1920x1080 previews regenerated and inspected without clipped action text | Pass |
| Package contents | One standalone ELF plus manifest, install note, license, and third-party notices | Pass |
| Local hygiene | No tracked token/private-key pattern, console IP, `strcpy`, `strcat`, or `sprintf` in active source | Pass |
| Repository | Repository now named `ItsBlurf/BFplayer`; authenticated remote verified | Pass |

The GCC analyzer also examined the host SQLite amalgamation and reported one
path-sensitive warning inside that generated third-party source. The project
does not modify the amalgamation, and the database regression suite passes.
Address/undefined sanitizers could not be linked in the installed Windows
MinGW environment because its sanitizer runtime libraries are absent; this is
an environment limitation, not a suppressed test failure.

GitHub currently reports Dependabot alerts, secret scanning, and code scanning
as disabled for this private repository. No repository setting was silently
changed during this source audit.

## Release artifacts

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer-standalone.elf` | 17,640,688 | `43feecd9ca3c77d6794d0d4630575a9e8659b436f7746f8e23cd8cc9eb199b92` |
| `BFplayer-standalone.zip` | 17,181,863 | `7b980ab2fec4075ef33d3d274b53cb246115bd4edaecef1f2cc853f48d21507b` |
| Embedded stripped player | 36,177,808 | `a82321ed6a5ece268583ba04c9c18b016c91bf8b6b2166e043127ce8437d34e4` |

The installed SDK release line was rechecked against official SDK v0.41, and
the packaged dependency line remains PacBrew v0.37. The final hardware gate is
the user's single clean controller/USB acceptance run after this prerelease is
published.
