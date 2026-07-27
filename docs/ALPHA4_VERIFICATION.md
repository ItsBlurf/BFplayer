# Alpha.4 verification record

Build date: 2026-07-23

## Host gates

- CMake/Ninja host build: pass
- CTest: 8/8 pass
  - library scanner
  - bounded local artwork discovery/header validation
  - Fit/Fill/Stretch display-layout math and setting parsing
  - library view/filter/sort
  - source URI redaction/persistence policy
  - bounded M3U/PLS/XSPF parsing and path resolution
  - websrv launcher URL validation
  - SQLite library/resume/preferences/Favorites
- `node --check homebrew.js`: pass
- `build.ps1`, `deploy.ps1`, and `stage-test-media.ps1` PowerShell parsing: pass

## PS5 binary gates

- Target: ELF64, x86-64, FreeBSD OS/ABI, DYN/PIE
- Entry point: `0x40`
- PacBrew: v0.37
- FFmpeg: 7.0.1
- Runtime dependencies: all 10 resolved by the installed SDK
  - `libkernel_web.sprx`
  - `libSceSystemService.sprx`
  - `libSceUserService.sprx`
  - `libScePad.sprx`
  - `libSceAudioOut.sprx`
  - `libSceVideoOut.sprx`
  - `libSceKeyboard.sprx`
  - `libSceImeDialog.sprx`
  - `libSceLibcInternal.sprx`
  - `libSceNet.sprx`

```text
d444842478ca707bad61a53aeeca898b7d08dd01e4d0d3d9c007b7786c788474  bfplayer.elf
f74d51a749ecc265c6aa37e2f4b56efac947b5d7fc94a5daced4be93bee83c86  BFplayer-websrv.zip
```

The ZIP contains exactly the six documented files. Each entry's SHA-256 was
compared against its staging source and matched.

## Media corpus gates

Local `ffprobe` inspection confirms the deterministic MKV contains H.264 video,
two tagged AAC tracks, two tagged SRT tracks, styled ASS, and three named
chapters. The separate sample confirms embedded PGS, and the extracted SUP
confirms a standalone PGS stream. All staging hashes match
[TEST_CORPUS.md](TEST_CORPUS.md).

## Hardware status

The configured `PS5_HOST=ps5` did not resolve on 2026-07-23. No upload or
deployment was attempted. This record proves the build/package gates only; it
does not prove VideoOut, AudioOut, controller, decode performance, A/V sync, or
the hardware ladder.
