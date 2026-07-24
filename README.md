# PS5 Media Center

PS5 Media Center is a native homebrew media player and library for the
open-source `ps5-payload-sdk`. It is a new implementation: the older
`ps5-media-player` and `ps5-media-fpkg` projects are retained only as failure
evidence and references.

![PS5 Media Center library preview](docs/ui-preview.png)

## Current foundation

- One BigApp-owned ELF with a direct dashboard tile that bypasses the websrv
  catalog and file picker.
- FFmpeg 7.0.1 demuxing and software decoding from PacBrew v0.37.
- SDL2 PS5 VideoOut, AudioOut, and controller backends.
- libass text/ASS subtitle rendering plus FFmpeg bitmap subtitles.
- Live embedded audio, subtitle, and video track switching.
- Pause/resume and 10/60/600-second seeking.
- Named chapter navigation, persistent volume/mute, resume positions, and OSD.
- VLC-compatible display controls: Best fit, Fill screen, Fullscreen stretch,
  all nine Desktop aspect choices, and all eleven Desktop crop choices.
- Per-title audio/subtitle selection, external-subtitle path, language fallback,
  and subtitle-delay persistence.
- Cancellable media-library indexing with All, Continue Watching, Recently
  Played, Movies, TV Shows, Music, and Playlists categories.
- PS5 on-screen-keyboard search across titles, filenames, paths, containers,
  and codec metadata, with cached multi-token filtering for large libraries.
- Persistent Smart, Name, Recently Played, Newest, Duration, and File Size
  sorting while keeping the current title selected as views reorder.
- Persistent Favorites with a dedicated category and transaction-safe updates
  even while the background scanner owns the SQLite write transaction.
- Native source selection from accessible PS5 storage: a selected file is
  presented as one movie, while a selected folder is persisted and grouped as
  one TV show with a naturally ordered episode view.
- Broad indexing for the confirmed PacBrew FFmpeg demuxers, including common
  media plus raw AV1/VVC, IVF, NUT, WTV, Bink/Smacker, R3D, HCA, QOA, and more.
- Direct HTTP(S)/HLS, FTP, RTSP, RTMP, RTP, TCP/UDP, SCTP, and MMST/MMSh URL
  launch from websrv, with a strict protocol allowlist and secret-safe logging.
- Cached background metadata probing for title, duration, resolution, container,
  and audio/video codecs; modified files are automatically re-probed.
- Selected-title local artwork with deterministic JPEG/PNG sidecar discovery,
  aspect-correct rendering, one-texture caching, and file/dimension limits.
- Explicit Play From Here queues for naturally advancing through episodes or
  albums while Options still returns immediately to the library.
- Bounded M3U, PLS, and XSPF playlist expansion with relative local paths,
  credential-safe network URLs, preserved ordering, and visible parse errors.
- Automatic matching sidecars plus explicit external SRT/ASS/WebVTT and
  PGS/SUP or IDX+SUB playback.
- Race-resistant local-file AVIO, descriptor-bound directory traversal,
  bounded network open/read deadlines, and rollback-safe partial scans.
- Transactional SQLite v1-to-v2 migration, bounded database text values, and
  last-good-index preservation after cancellation or storage errors.
- A project-owned hardened embedded bitmap-subtitle renderer that validates
  palettes, indices, rectangles, cache growth, callbacks, and cleanup.
- Persistent rotating runtime diagnostics under
  /data/PS5-MediaCenter/logs, including FFmpeg/SDL/libass messages,
  source/track/scan events, five-second playback heartbeats, and fatal-signal
  markers; see docs/DIAGNOSTICS.md.
- No background launcher daemon, process sweeps, or SystemService calls made
  by application code.

The PS5 SDL backend currently renders and decodes in software. Format support
is broad, but high-bitrate 4K HEVC/AV1 performance must be measured on hardware
and must not be advertised as guaranteed.

## Build

The workspace SDK environment and the verified PacBrew v0.37 target libraries
are required. From this folder:

```powershell
. ..\..\ps5dev-env.ps1
.\build.ps1
```

`build.ps1` locates PacBrew beside the installed SDK by default:

```text
<toolchains>\pacbrew-v0.37\homebrew
```

Override that location with `PS5_PACBREW_HOME` if needed. The build output is
`dist\ps5-media-center.elf`; the primary ready-to-copy package is
`dist\PS5-MediaCenter-direct-tile.zip`. The compatibility-only catalog package
remains `dist\PS5-MediaCenter-websrv.zip`.

## Install

If PS5 FTP is running on port 2121:

```powershell
.\deploy.ps1
.\stage-test-media.ps1
```

The script only uploads this product's seven package files and writes a deploy
log under `logs/deploy`. The second script verifies and uploads the deterministic
multitrack/subtitle corpus to `/data/media/PS5MC-Test` for the hardware ladder;
neither script deletes remote files. For a manual install, extract the
direct-tile package so the final layout is
`/data/homebrew/PS5-MediaCenter/...`, start websrv on port 8080, and inject the
included `ps5mc-tile-installer.elf` once. After that, the **PS5 Media Center**
dashboard tile calls the exact player ELF directly; the HBL catalog and picker
are never displayed. See [docs/DIRECT_TILE.md](docs/DIRECT_TILE.md).

After any test, run collect-logs.ps1 with the PS5 IP before launching another
run. It downloads latest.log and previous.log into a timestamped folder. See
docs/DIAGNOSTICS.md for the collection procedure.

## Optional future console validation

No console connection, upload, deployment, or runtime test is part of the
current verification scope. If the owner later chooses to validate it, use the
direct tile so websrv creates the required BigApp without showing its catalog.
Do not send this ELF to a background elfldr process and do not run it alongside
an older media-player launcher.

Controller mappings during direct-file playback:

| Button | Action |
| --- | --- |
| Cross | Pause/resume |
| Circle | Cycle embedded subtitles, including Off |
| Square | Cycle audio tracks |
| Triangle | Cycle video tracks |
| R2 + Triangle | Cycle VLC display-aspect ratios: Default, 1:1, 4:3, 16:9, 16:10, 2.21:1, 2.35:1, 2.39:1, 5:4 |
| L2 + Triangle | Cycle VLC crop ratios: Default, 16:10, 16:9, 1.85:1, 2.21:1, 2.35:1, 2.39:1, 5:3, 4:3, 5:4, 1:1 |
| L2 + R2 + Triangle | Cycle Best fit, Fill screen (crop), and Fullscreen stretch |
| D-pad Left/Right | Seek -/+10 seconds |
| D-pad Down/Up | Seek -/+60 seconds |
| L1/R1 | Previous/next chapter, or -/+600 seconds when none exist |
| L3/R3 | Volume down/up |
| Create | Mute/unmute |
| Touchpad | Toggle external-subtitle delay mode; D-pad adjusts timing |
| Options | Return/exit |

Controller mappings in the media library:

| Button | Action |
| --- | --- |
| Cross | Play selected item |
| Circle | Play from here in the current filtered/sorted view |
| Triangle | Cycle library category |
| D-pad Left/Right | Previous/next library category |
| Touchpad | Open the Add Media Source browser |
| Create | Clear the active search |
| L3 | Add/remove the selected item from Favorites |
| R3 | Cycle and persist the library sort mode |
| Square | Rescan configured storage roots |
| D-pad Up/Down | Move one item |
| L1/R1 | Move one page |
| Options | Exit |

Inside Add Media Source, Cross opens a folder or adds the highlighted media
file as a standalone movie. Triangle adds the highlighted folder as one TV
show, Square adds the current folder as one TV show, Circle goes to the parent
folder, and Options closes the browser.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/VLC_DISPLAY_MODES.md](docs/VLC_DISPLAY_MODES.md), and
[docs/HARDWARE_TEST_LADDER.md](docs/HARDWARE_TEST_LADDER.md) before deployment.
The exact static-library evidence is recorded in
[docs/FFMPEG_SYMBOL_AUDIT.md](docs/FFMPEG_SYMBOL_AUDIT.md), and the remaining
acceptance gates are tracked in
[docs/COMPLETION_MATRIX.md](docs/COMPLETION_MATRIX.md).
The current alpha is exhaustively offline-verified but cannot claim console
runtime behavior; the owner explicitly required that the PS5 remain untouched.
See [docs/FULL_VERIFICATION_AUDIT.md](docs/FULL_VERIFICATION_AUDIT.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
