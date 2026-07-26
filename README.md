# PS5 Media Center

PS5 Media Center is a native homebrew media player and library for the
open-source `ps5-payload-sdk`. It is a new implementation: the older
`ps5-media-player` and `ps5-media-fpkg` projects are retained only as failure
evidence and references.

![PS5 Media Center library preview](docs/ui-preview.png)

## Current foundation

- One standalone resident payload containing the player, dashboard-tile
  installer, runtime assets, minimal loopback launcher, and BigApp transition.
- One `PSMC00001` Media-category registration serves as both the visible tile
  and repaired BigApp runtime host, with no second Games entry.
- Stripped, gzip-compressed embedded player for a much smaller LAN-injected
  payload, expanded and ELF-validated in memory only when launched.
- No websrv runtime dependency, port 8080, web catalog, or web file picker:
  the English-only tile calls a private `127.0.0.1:9040/launch` route, and the
  native in-app source browser starts at `/`.
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
- Cancellable indexing of only the movie files and TV folders manually chosen
  by the user, with All, Continue Watching, Recently Played, Movies, TV Shows,
  Music, and Playlists categories.
- PS5 on-screen-keyboard search across titles, filenames, paths, containers,
  and codec metadata, with cached multi-token filtering for large libraries.
- Persistent Smart, Name, Recently Played, Newest, Duration, and File Size
  sorting while keeping the current title selected as views reorder.
- Persistent Favorites with a dedicated category and transaction-safe updates
  even while the background scanner owns the SQLite write transaction.
- Native source selection from accessible PS5 storage: a selected file is
  presented as one movie, while a selected folder is persisted and grouped as
  one TV show with a naturally ordered episode view.
- Explicit whole-library import for a chosen folder: loose video files become
  Movies and immediate child folders containing video recursively become TV
  Shows. No storage root is scanned automatically.
- Two-press removal of a selected Movie or TV Show source from the library.
- Broad indexing for the confirmed PacBrew FFmpeg demuxers, including common
  media plus raw AV1/VVC, IVF, NUT, WTV, Bink/Smacker, R3D, HCA, QOA, and more.
- FFmpeg support for HTTP(S)/HLS, FTP, RTSP, RTMP, RTP, TCP/UDP, SCTP, and
  MMST/MMSh sources, with a strict protocol allowlist and secret-safe logging.
- Cached background metadata probing for title, duration, resolution, container,
  and audio/video codecs; modified files are automatically re-probed.
- Selected-title local artwork with deterministic JPEG/PNG sidecar discovery,
  aspect-correct rendering, one-texture caching, and file/dimension limits.
- Bounded background extraction of an interior video frame when no usable
  cover sidecar exists, with alternate positions used to avoid black frames.
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
- The player itself has no AppInst privileges. Privileged tile registration
  and the BigApp transition stay isolated in the resident launcher payload.

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
`dist\ps5-media-center-standalone.elf`; the ready-to-copy archive is
`dist\PS5-MediaCenter-standalone.zip`. The standalone ELF already embeds the
compressed player ELF, font, icon, single-title Media registration, and launch
core.

## Install

Inject `dist\ps5-media-center-standalone.elf` once after each jailbreak. That
single payload installs or refreshes the **PS5 Media Center** dashboard tile,
materializes its small runtime assets, and remains resident to service the
tile. No Media Center folder transfer and no websrv payload are required.
The payload recreates `/data/homebrew/PS5-MediaCenter`; that folder is runtime
material, not a prerequisite.
See [docs/STANDALONE_LAUNCHER.md](docs/STANDALONE_LAUNCHER.md).

After any test, run collect-logs.ps1 with the PS5 IP before launching another
run. It downloads latest.log and previous.log into a timestamped folder. See
docs/DIAGNOSTICS.md for the collection procedure.

## Optional future console validation

No console connection, upload, deployment, or runtime test is part of the
current verification scope. Hardware validation should inject only the
standalone payload, confirm its ready log, then select its dashboard tile. Do
not stack an older Media Center launcher during the same test.

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
| D-pad Left/Right | Seek by the configurable short step (10 seconds by default) |
| D-pad Down/Up | Seek by the configurable long step (60 seconds by default) |
| L1/R1 | Previous/next chapter, or -/+600 seconds when none exist |
| L3/R3 | Volume down/up |
| Create | Mute/unmute |
| Touchpad | Show the complete playback control map |
| Options | Open the playback menu (controls, subtitle timing, return to library) |

Controller mappings in the media library:

| Button | Action |
| --- | --- |
| Cross | Play selected item |
| Circle | Play from here in the current filtered/sorted view |
| Triangle | Remove the selected Movie/TV Show source (press twice) |
| D-pad Left/Right | Previous/next library category |
| Touchpad | Open the Add Media Source browser |
| Create | Clear the active search |
| L3 | Add/remove the selected item from Favorites |
| R3 | Cycle and persist the library sort mode |
| D-pad Up/Down | Move one item |
| L1/R1 | Move one page |
| Options | Open Add Media, Controls, Playback Settings, About, or Exit |

Inside Add Media Source, Cross opens a folder or adds the highlighted media
file as a standalone movie. Triangle adds the highlighted folder as one TV
show. Square explicitly imports the highlighted folder (or the current folder
when a file is highlighted) as a whole library: loose video files become
Movies and child folders containing videos become TV Shows.
The browser always starts at `/`; Circle goes to the parent (or closes at `/`)
and Options closes it.

Playback Settings in the Options menu controls default volume, short and long
seek steps, on-screen-display duration, resume behavior, and automatic
subtitle selection. Changes are saved immediately and apply to the next item
opened. The Controls page is also available directly with F1 on desktop builds.

See [docs/STANDALONE_LAUNCHER.md](docs/STANDALONE_LAUNCHER.md),
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/VLC_DISPLAY_MODES.md](docs/VLC_DISPLAY_MODES.md), and
[docs/HARDWARE_TEST_LADDER.md](docs/HARDWARE_TEST_LADDER.md) before deployment.
The exact static-library evidence is recorded in
[docs/FFMPEG_SYMBOL_AUDIT.md](docs/FFMPEG_SYMBOL_AUDIT.md), and the remaining
acceptance gates are tracked in
[docs/COMPLETION_MATRIX.md](docs/COMPLETION_MATRIX.md).
The current alpha is exhaustively offline-verified but cannot claim console
runtime behavior. See
[docs/ALPHA13_OFFLINE_VERIFICATION.md](docs/ALPHA13_OFFLINE_VERIFICATION.md),
[docs/FULL_VERIFICATION_AUDIT.md](docs/FULL_VERIFICATION_AUDIT.md), and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
