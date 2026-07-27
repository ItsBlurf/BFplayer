# Alpha.10 standalone offline verification

Version: `0.1.0-alpha.10`

This build replaces the alpha.9 websrv launch path with a single resident
payload. No PS5 connection, deployment, or runtime launch was performed during
this verification.

## Requirement evidence

| Requirement | Evidence | Result |
| --- | --- | --- |
| One PS5 payload | Release archive contains one `.elf`: `bfplayer-standalone.elf` | Pass |
| Player embedded | Byte-for-byte verifier found the complete 56,268,760-byte player ELF exactly once inside the standalone ELF | Pass |
| Runtime assets embedded | Tile JSON, icon, Noto Sans font, and OFL text each occur byte-for-byte exactly once | Pass |
| No websrv runtime | No websrv source/header, libmicrohttpd link, websrv package, `homebrew.js`, or separate tile installer remains in the active build | Pass |
| No port 8080 | Tile and launcher use `127.0.0.1:9040`; final ELF string audit contains no `8080` or `/hbldr` route | Pass |
| Minimal private service | Source binds `INADDR_LOOPBACK`, accepts only exact `GET /launch`, and rejects query strings, other methods, and other paths | Pass |
| English only | Tile parameters contain only `defaultLanguage` and `en-US` | Pass |
| BigApp transition retained | Embedded player is passed directly to the retained `hbldr`/ptrace/ELF replacement core | Source/build pass |
| PS5 target | ELF64, little-endian, FreeBSD OS/ABI, x86-64, ET_DYN, entry `0x40` | Pass |

## Test and build results

- 11/11 host tests passed.
- Exact-route tests cover GET, POST, query, suffix, root, empty, and null
  requests.
- PS5 target player and standalone launcher compiled and linked through the
  workspace SDK wrappers with no warnings.
- SDK dependency verification passed for all seven outer payload dependencies:
  `libkernel_sys`, `libSceSystemService`, `libSceUserService`,
  `libSceAppInstUtil`, `libSceLibcInternal`, `libSceNet`, and
  `libkernel_web`.
- `libkernel_web.sprx` is the SDK's low-level runtime stub. It is not the
  ps5-payload-websrv application and does not imply a port-8080 service.
- PowerShell parsing passed for build, deploy, and log-collection helpers.
- `git diff --check` passed.

The retained BigApp loader files are based on ps5-payload-websrv commit
`baabe27e5449baeb059b850d0393c31fdee219b7`. Their GPL-3.0-or-later notices,
modification note, complete license, and source are included.

## Final artifact hashes

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` (embedded player intermediate) | 56,268,760 | `cf5c4e194a60f4ed0586cb1434e9688da9cebf2b1d60996140cb4a9ca104468a` |
| `bfplayer-standalone.elf` | 58,401,352 | `8408ad0c2ce920b3e5ec0824c9a372bc3646851c6231a4372318f51dfd1d7681` |
| `BFplayer-standalone.zip` | 24,530,266 | `2d38568988824187297a070edce2ba46905f8303b5738db393608d304d24a1d3` |
| `build-manifest.json` | 1,563 | `81a0726501754fc85c14460ae8924cc53896deec4ac82756c51550edf42c03f1` |

## Unverified hardware boundary

Offline evidence proves the package structure, embedded bytes, route policy,
target ABI, imports, and build integrity. It does not prove that the dashboard
request, firmware-specific FakeApp creation, BigApp replacement, SDL VideoOut,
or AudioOut succeeds on a specific console. The first PS5 run must be treated
as a test build and the standalone launcher/player logs must be collected
before any repeat launch after a black screen or crash.
