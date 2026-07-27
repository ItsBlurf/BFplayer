# Alpha.13 offline verification

Version: `0.1.0-alpha.13`

This test build removes the duplicate Games registration introduced by the
private alpha.11/12 runtime host. New dashboard/runtime behavior requires PS5
hardware validation.

## Results

| Check | Evidence | Result |
| --- | --- | --- |
| One title ID | Dashboard registration, repaired system host, and BigApp launch all use `PSMC00001` | Pass |
| Media category only | The standalone ELF embeds exactly two `applicationCategoryType: 65536` records (tile and system host) and no category `0` record | Pass |
| Legacy migration | Startup calls Sony AppInst uninstall for `PSMR00001`, waits for app-database processing, and removes known stale `/user/app/PSMR00001` metadata so fallback discovery cannot reinstall it | Pass |
| No HBL dependency | The system host remains project-owned under `/system_ex/app/PSMC00001`; `FAKE00000`, websrv, and port 8080 are absent | Pass |
| Add Media root | The interactive picker opens at `/`, permits explicit mount traversal, retains no-follow checks, and reports failures in the UI | Pass |
| Bulk import bounds | Direct and combined traversal budgets reject oversized or incomplete imports instead of publishing partial source lists | Pass |
| Host tests | All 12 CTest targets passed | Pass |
| PS5 target build | SDK wrappers produced x86-64 FreeBSD-ABI ET_DYN player and standalone ELFs | Pass |
| Standalone embedding | Gzip expands byte-for-byte to the stripped player; expected assets occur exactly once | Pass |
| SCE imports | All 10 player and 7 standalone dependencies resolve in the installed payload SDK | Pass |

## Final artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,128,656 | `739a95c2b1409fd63cf040fde87f33b197611e49431b941f6cd1f98872d26b86` |
| `bfplayer.elf.gz` (embedded intermediate) | 15,440,108 | `9725da508d94753ede85395d74138bcb49ac0fd74dbb6c1c825bd4731cd6db41` |
| `bfplayer-standalone.elf` | 17,607,712 | `3e077c79ddfedbdab041f827c249033452a6f0aa09a0c4d5182d03f7cc660ef7` |
| `BFplayer-standalone.zip` | 17,161,635 | `c30da2db8337e86f6628bfdef0d7f357da8967d03d7f2d4a56a58c388a9cfbd0` |

## Hardware gate

End the resident alpha.12 launcher before injecting alpha.13. Confirm that
`PSMR00001` disappears from Games, exactly one BFplayer tile remains
in Media, and that launching the consolidated `PSMC00001` BigApp host still
opens and exits the player correctly.
