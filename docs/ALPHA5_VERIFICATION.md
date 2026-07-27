# Alpha 5 offline verification

Version: `0.1.0-alpha.5`
Build UTC: `2026-07-23T22:07:28.3085101Z`

## Verification boundary

The PS5 was intentionally kept offline. No hostname lookup, port probe, FTP
connection, upload, deployment, payload injection, or console run was
attempted. The results below establish a reproducible PS5-target build and
offline correctness evidence; they do not claim observed console performance
or runtime behavior.

## Final gates

| Gate | Result |
| --- | --- |
| PowerShell parser | `build.ps1`, `deploy.ps1`, and `stage-test-media.ps1` passed |
| JavaScript parser | `node --check homebrew.js` passed |
| Clean host Release build | Configured in a new Ninja build directory with GCC 16.1.0 |
| Host tests | 9/9 passed, 0 failed, 0.29 seconds |
| Authored target static analysis | All 14 C/C++ translation units analyzed through the PS5 wrappers |
| SDL_kitchensink target static analysis | All 25 local upstream translation units analyzed |
| PS5 Release build | All 14 authored objects rebuilt and linked |
| Target identity | ELF64, little-endian, x86-64, FreeBSD OS/ABI 9, DYN/PIE, entry `0x40` |
| Runtime dependencies | Exact expected 10 SCE SPRX dependencies; all available in SDK |
| Runtime imports | 371 unique undefined ELF symbols; 0 missing from the 10 selected SDK stubs |
| Bitmap subtitle override | Exactly one `Kit_CreateImageSubtitleRenderer` definition in the ELF, supplied by `kitsubimage_safe.o` |
| FFmpeg archives | 353 demuxers, 500 decoders, 42 protocols |
| Media corpus | All six published SHA-256 hashes and expected stream topology passed |
| Package | Exact seven-file ZIP inventory; every entry matches staging and manifest |
| Runtime diagnostics | Persistent rotating logs, bounded FFmpeg/SDL/libass capture, playback heartbeats, and fatal-signal markers |

The authored-code analyzer's only remaining diagnostic is a libc++ `std::sort`
false positive: it treats the implementation's valid moved-from pivot as
invalid when calling the comparator. The comparator has deterministic
irreflexive, asymmetric, and transitive property coverage. The descriptor
diagnostics found during the first final pass were fixed by checking `dirfd()`
before every `fstatat()` call and the affected units were reanalyzed.

A sanitizer build was attempted separately, but the installed MinGW toolchain
does not contain `libasan`/`libubsan`, and the installed clang-cl environment
does not contain the required MSVC SDK libraries. This document does not claim
a sanitizer pass.

## ELF and package artifacts

```text
e85217f31a618e327c2d5c56e08da68d2a0d3f109d3cad7c9cadf2f263cc21a3  bfplayer.elf
e1dbb2ed29356ac6bb169a14de447587342fb4590a65c5d948efe6a169d48131  BFplayer-websrv.zip
```

| Artifact | Bytes |
| --- | ---: |
| `bfplayer.elf` | 56,176,504 |
| `BFplayer-websrv.zip` | 24,186,016 |

The ELF has these exact `DT_NEEDED` entries:

```text
libkernel_web.sprx
libSceSystemService.sprx
libSceUserService.sprx
libScePad.sprx
libSceAudioOut.sprx
libSceVideoOut.sprx
libSceKeyboard.sprx
libSceImeDialog.sprx
libSceLibcInternal.sprx
libSceNet.sprx
```

## Package file manifest

The ZIP contains one directory entry plus these exact seven files under
`BFplayer/`:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `build-manifest.json` | 1,810 | `1649210c46e0b16017a27f2607eb2ea25f5f56bc3a260463efba7c8ef26941c6` |
| `homebrew.js` | 3,775 | `e2d060a1e422135bff67de0f04019814872bc2e418401a6e03962c24269473c6` |
| `bfplayer.elf` | 56,176,504 | `e85217f31a618e327c2d5c56e08da68d2a0d3f109d3cad7c9cadf2f263cc21a3` |
| `THIRD_PARTY_NOTICES.md` | 3,616 | `1a58959fe7b47bb249c67f417363375dc69673735ceef2a0d275fa5ac1688340` |
| `assets/fonts/NotoSans-Regular.ttf` | 569,208 | `b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5` |
| `assets/fonts/OFL.txt` | 4,377 | `0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb` |
| `sce_sys/icon0.png` | 1,163,491 | `2b5f1f1c9e21ef5ec8170bd4a081f69623b39fe439fe834b9bde1f4a5a32d5ae` |

The six payload-file records in `build-manifest.json` match the staging files
and ZIP entries byte-for-byte. The manifest's top-level ELF hash and the two
standalone `.sha256` files also match independently calculated hashes.

## Deterministic media evidence

All hashes in [TEST_CORPUS.md](TEST_CORPUS.md) passed. Independent `ffprobe`
inspection found:

- `ps5mc-multitrack-test.mkv`: one 1280x720 H.264 video stream, two AAC audio
  streams, two SubRip plus one ASS subtitle stream, three chapters, and
  duration `10.021000` seconds;
- `ffmpeg-pgs-supsample.mkv`: one embedded `hdmv_pgs_subtitle` stream;
- `ps5mc-multitrack-test.bitmap.sup`: one external
  `hdmv_pgs_subtitle` stream.

The full claim boundary, implementation mapping, fixed defects, and residual
limits are recorded in [FULL_VERIFICATION_AUDIT.md](FULL_VERIFICATION_AUDIT.md).
