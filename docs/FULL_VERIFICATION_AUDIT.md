# Full offline verification audit

Scope date: 2026-07-23
Build line: `0.1.0-alpha.5`

## Scope and claim boundary

The owner required that the PS5 remain offline and never be contacted for this
work. No hostname probe, FTP connection, upload, deployment, elfldr injection,
or console run was attempted. This audit therefore supports an
**offline-verified PS5 build**, not a claim that VideoOut, AudioOut, controller
input, A/V sync, throughput, or every codec has been observed on hardware.

Offline completion uses independent evidence layers:

1. authored-source review and target-aware static analysis;
2. deterministic host tests for platform-independent state and parsing;
3. compilation and static linking for `x86_64-sie-ps5`;
4. ELF header, import, dependency, and symbol inspection;
5. exact FFmpeg archive capability enumeration;
6. deterministic media-corpus inspection and hashes;
7. exact release-archive inventory and per-file hashes.

The final command results and artifact hashes are recorded in
[ALPHA5_VERIFICATION.md](ALPHA5_VERIFICATION.md).

## Requirement evidence

| Area | Implementation evidence | Offline verification |
| --- | --- | --- |
| Broad playback | FFmpeg 7.0.1 demux/codec archives, Kitchensink player, SDL2 output | 353 demuxers, 500 decoders, 42 protocols enumerated from defined archive symbols; final link and imports checked |
| Local media safety | `SafeReadFile`, custom FFmpeg AVIO, open identity checks | Host regular/symlink/directory/seek tests; PS5-target compile/analyzer |
| Network media | Scheme and credential policy, redacted logging, deadlines, protocol whitelist | C++ URI tests, launcher JavaScript tests, source audit, linked protocol audit |
| Audio/video switching | Live decoder replacement plus transactional SDL resource rebuild/rollback | Target analyzer and clean PS5 build; runtime behavior intentionally unobserved |
| Embedded subtitles | Kitchensink text path plus project-owned hardened bitmap renderer override | Replacement symbol selected once in final ELF; unsafe upstream archive member not selected |
| External subtitles | Independent bounded FFmpeg demux/decode, libass text, validated bitmap conversion | Deadline/packet/event/byte/cache bounds audited; target analyzer/build |
| Playback state | Resume, completion, volume, mute, chapters, scale, track/language and subtitle-delay persistence | SQLite round-trip/migration tests; integer/finite-value audit |
| Library | Bounded no-follow scanner, metadata cache, search, categories, sort, Favorites, queue | Host scanner/view/database/artwork tests; descriptor-relative PS5 branch compiles and analyzes |
| Playlists | Bounded M3U/PLS/XSPF parser, relative paths, URL filtering | Host parsing/limit/error tests and target build |
| Packaging | Single HBL BigApp ELF and websrv folder | Exact seven-file inventory, manifest per-file hashes, ZIP/source hash comparison |
| Diagnostics | Rotating persistent on-console log with SDL/FFmpeg/libass capture, playback heartbeats, scan/database events, and crash markers | Target build and static analysis; collection procedure in DIAGNOSTICS.md |

## Defects removed during final audit

- Replaced local media URL opening with no-follow regular-file AVIO.
- Added cancellable 30-second source/subtitle and five-second metadata probe
  deadlines with bounded probe budgets and protocol allowlists.
- Guarded a null `Kit_CloseSource` path that upstream asserts/dereferences.
- Made audio-only files with lyric/subtitle streams avoid an invalid
  Kitchensink subtitle decoder configuration.
- Made audio/video/subtitle track resource changes transactional with rollback.
- Corrected float-audio storage alignment, bounded decoder pulls to remaining
  queue capacity, validated sample-byte alignment, and checked queue failures.
- Prevented failed external sidecars from disabling a working embedded track.
- Hardened embedded and external bitmap subtitle palette, index, rectangle,
  callback, allocation, and cleanup behavior.
- Removed signed-overflow paths in subtitle timestamps, completion/progress
  calculations, corrupt saved volume parsing, and library resume percentages.
- Added transactional SQLite schema migration, future-version rejection,
  pre-open state checks, text/row caps, and bounded text extraction.
- Changed partial/cancelled/error scans to roll back instead of deleting cached
  rows that were not revisited.
- Bound PS5 directory entry checks to opened descriptors with
  `fstatat(AT_SYMLINK_NOFOLLOW)`, prevented device crossing, and fixed stale
  `errno` being mistaken for a `readdir` failure.
- Added no-follow and identity validation to primary local media, metadata,
  playlists, artwork, and standalone subtitle files.
- Added a third-party notice and made package staging reject any missing or
  unexpected file.
- Added bounded persistent diagnostics with previous-session rotation, redacted
  paths, installed-manifest capture, decoder/library event records, and
  signal-safe fatal markers.

## Static-analysis record

- Clang static analyzer ran through the PS5 compiler wrappers on all 14 authored
  C/C++ translation units.
- One diagnostic originated inside libc++ `std::sort`: it considered a valid
  moved-from internal pivot suspicious while invoking the string comparator.
  Deterministic strict-order property tests cover the comparator; no project
  memory, bounds, null, leak, lifetime, or descriptor defect was reported.
- The same target analyzer was also run on all 25 local SDL_kitchensink source
  translation units. The application still overrides the upstream bitmap
  renderer because manual review found contract and cleanup defects that the
  analyzer does not model.
- A Windows sanitizer build was attempted. The installed MinGW distribution
  lacks `libasan`/`libubsan`, and the standalone clang-cl installation lacks
  the MSVC SDK libraries, so sanitizer execution was unavailable. This is a
  toolchain limitation, not a passing sanitizer claim.

## Explicit residual limits

- All video decoding is software; high-bitrate 1080p, HEVC/VP9/AV1, 10-bit,
  and 4K performance cannot be established offline.
- Runtime PS5 SDL, AudioOut, VideoOut, IME, controller, USB mount, and network
  behavior is unobserved by instruction.
- Primary local inputs are no-follow and descriptor-backed. Formats that ask
  FFmpeg to open declared companion files internally (notably local HLS
  segments and IDX+SUB data) retain restricted `file` protocol access so those
  formats remain usable.
- External subtitle files are decoded before playback and have explicit
  30-second, packet/event, 64 MiB text, and 256 MiB bitmap-cache ceilings.
- Playback rate control, selectable deinterlacing, disc menus, DRM, HDMI
  passthrough, embedded artwork extraction, and online metadata are not
  implemented and are not claimed.
