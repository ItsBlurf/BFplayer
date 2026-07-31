# Alpha 44 verification

Build: `0.1.0-alpha.44`

Release artifacts:

- Standalone ELF SHA-256:
  `0d028deb145d6fc9a5b55d43a45e072919178fbb261c66cd914ebcfb0b3b05c0`
- Standalone ZIP SHA-256:
  `eaf7f16965f28b339384df6989f9354e0996ebb71affbfd8e6cc8dcd13b0af2c`
- websrv ZIP SHA-256:
  `5b6d3bb25ae8127c18e8909f81db795d0e43ea2347f6b86eb02969ed67350852`

## Offline gates

- PS5 standalone and websrv players build from the same source tree.
- The standalone launcher embeds the current stripped player.
- The websrv package contains the expected eight files and passes the package
  verifier.
- All 25 host tests and the structural launcher tests pass.
- The regenerated PS5 SDL backend patch applies to its pinned upstream commit
  and its checked-in object matches the recorded SHA-256.

## PS5 hardware evidence

| Test | Result |
| --- | --- |
| 3840x2160 59.94 fps VP9 Profile 2 PQ | Native 3840x2160 HDR10; about 59.9 fps; zero steady misses |
| 3840x2160 59.94 fps HLG presentation workload | Native HLG-to-PQ HDR10; HLG tables prewarmed in 9.5 ms; about 59.9 fps after warm-up; zero steady misses |
| Accurate 4K60 Main 10 HEVC HLG probe | Native HDR presentation averaged about 11.4 ms; software decode limited overall delivery to about 47.8 fps |
| Non-16:9 PQ source | Native 3840x2160 output completed at real-time speed with the source display ratio preserved |
| WebM absolute and relative seeks | Requested positions became visible within the first status sample |
| Dandadan through PC DLNA server | 1920x1080 playback held 24 fps; nested server browsing and direct media GETs succeeded |
| DLNA seek | Requested 120 s position observed at 120.189 s within 300 ms; steady playback recovered |
| Subtitle change | New stream active within 200 ms; queued audio remained between 36 and 64 KiB in the isolated steady-state check |
| Audio change | New stream active within 200 ms; its queue drained the previous track and refilled during resumed playback |
| Ghost in the Shell through DLNA | 1920x1040 playback held its 23.976 fps source rate |

The PQ and HLG measurements use the same VP9 decode workload to isolate
presentation cost. The accurate HEVC HLG file is retained as separate evidence
that the HLG metadata and conversion path open correctly; its remaining limit
is software HEVC decode throughput.

## Launcher and package behavior

- Clean injection removes the active player and exact prior resident launcher,
  then leaves one replacement launcher, one player, and one Media tile.
- A PS5-side loopback probe received `HTTP/1.1 204 No Content` with
  `Content-Length: 0`; the HTML page is now failure-only.
- The optional websrv ZIP contains the same release player and does not include
  or depend on BFplayer's resident standalone launcher.
