# Alpha.11 offline verification

Version: `0.1.0-alpha.11`

This build fixes the standalone launcher's accidental dependence on HBL's
registered `FAKE00000` app and reduces the LAN-injected payload size. The PS5
was intentionally offline, so no console deployment or runtime claim is made.

## Results

| Check | Evidence | Result |
| --- | --- | --- |
| PS5 target build | SDK wrappers produced x86-64 FreeBSD-ABI ET_DYN player and standalone ELFs with no compiler warnings | Pass |
| Host tests | All 11 CTest targets passed | Pass |
| Embedded player | Gzip expands byte-for-byte to the stripped player; the raw player is not duplicated in the standalone ELF | Pass |
| Embedded assets | Tile param, private-host param, icon, font, and font license each occur exactly once | Pass |
| Runtime independence | Source owns `PSMR00001`, repairs `/system_ex/app/PSMR00001`, registers `/user/app/PSMR00001`, and refreshes `/data/homebrew/BFplayer` on startup and before every launch | Pass |
| HBL isolation | Active source contains no `FAKE00000`, HBL picker, websrv route, port 8080, or libmicrohttpd dependency | Pass |
| Launch fail-safety | BigApp creation and previous-app termination waits are bounded and logged | Pass |
| SCE imports | All 10 player and 7 standalone SCE dependencies are present in the installed payload SDK | Pass |
| UI preview | Exact dashboard artwork is used in-app; footer uses rendered DualSense-style control prompts | Pass |

## Final artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer.elf` | 36,095,888 | `392453514af1cb6266efe4a776afca4872bd66eca8abe3d0cc0eb0a10807b3ad` |
| `bfplayer.elf.gz` (embedded intermediate) | 15,423,329 | `03ac235035bf8e26058caaf9cf5f11c85c57843fc375faa7726674e82b4db51a` |
| `bfplayer-standalone.elf` | 17,591,320 | `4e850c7589ec01b5fea1206d7984f58cbe743d1c2a8ca78780de10bc8cb49ed3` |
| `BFplayer-standalone.zip` | 17,143,955 | `6b685e2a4c0f90c42f2b4de077638e5d982c8be4bd8dd342b36cb68e2bd8ccc7` |

The injectable standalone ELF is 40,810,032 bytes (69.9%) smaller than the
58,401,352-byte alpha.10 standalone ELF.

## Hardware gate

The remaining acceptance test is a clean PS5 run with HBL uninstalled and
`/data/homebrew/BFplayer` absent before injection. Follow
`HARDWARE_TEST_LADDER.md` when the console is available and collect all
launcher/player logs before another run.
