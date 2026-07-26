# Architecture

## Decision

The release is one resident Prospero payload. It embeds the complete stripped
player ELF as gzip, uses the single Media-category `PSMC00001` title as both
the dashboard tile and BigApp host, and binds a one-route HTTP listener only to
`127.0.0.1:9040`. The tile calls `/launch`; the resident payload expands and
validates the player, creates the BigApp context, and replaces it directly.

The implementation retains only the proven GPL BigApp/ptrace/ELF-loader core.
It does not embed or depend on the websrv application, libmicrohttpd, port
8080, the catalog, `homebrew.js`, or the HBL picker.

```text
standalone payload -> loopback :9040 <- PS5 Media Center tile
        |                 |
 compressed player   PSMC00001 BigApp transition
        |                 |
        +---------> BigApp player
                          |
              +-----------+---------------------+
              |                |                |
          Library UI      Playback engine    Persistence
          SDL2/TTF        Kitchensink/FFmpeg SQLite
              |                |
          PS5 Pad          libass + SDL2
                               |
                       VideoOut / AudioOut
```

There is no daemon/child-player boundary. Playback and the media library share
one UI process, so exiting a movie returns to the library without killing or
restarting unrelated PS5 processes.

## Dependencies

| Component | Version/source | Role |
| --- | --- | --- |
| FFmpeg | 7.0.1, PacBrew v0.37 | Containers, codecs, network I/O, metadata |
| SDL2 | PS5 port, PacBrew v0.37 | Window, software textures, AudioOut, Pad |
| SDL_image | PacBrew v0.37 | Bounded JPEG/PNG local artwork decoding |
| SDL_kitchensink | 2.0.0-a2 + PS5 PTS patch | Decode queues, sync, seeking, track switching |
| libass | 0.17.3 | Styled text subtitles and embedded fonts |
| SQLite | PacBrew v0.37 | Library index, resume position, preferences |
| BigApp/ELF loader core | ps5-payload-websrv, GPL-3.0-or-later | Minimal in-memory BigApp transition |

The project links these statically. SCE system libraries remain dynamic stubs,
matching the official PacBrew FFplay binary.

## Playback capabilities

Kitchensink exposes live stream replacement for video, audio, and embedded
subtitle streams. Switching preserves the common player clock and does not
reopen the container. Audio output is requested as 48 kHz stereo signed 16-bit
because the current PS5 SDL backend supports mono/stereo S16 output; multichannel
sources are downmixed by FFmpeg.

The video renderer separates decoded pixel dimensions from the stream display
aspect ratio. Pure, host-tested layout math independently applies the complete
VLC Desktop aspect-override list, the complete VLC Desktop crop-ratio list,
and Best fit, Fill screen/center-crop, or Fullscreen stretch. The trigger axes
modify Triangle while preserving Triangle alone for video-track switching.
Scale, aspect, and crop selections are stored as global SQLite settings.

The selected audio and subtitle stream indices are persisted per media path.
Language tags are stored alongside the indices so a remux can recover the same
language even when stream numbering changes. External subtitle paths and timing
offsets are restored as well; an explicit `--subtitle` argument always wins.

Network sources pass through FFmpeg unchanged. The launcher uses an explicit
scheme allowlist and rejects username/password authority fields. The runtime
redacts URL credentials, query strings, and fragments from logs. It also skips
per-title resume and track-preference persistence for such sensitive URLs so a
signed stream token cannot be written to SQLite; global volume persistence is
still retained. Network open and read operations have interrupt deadlines,
probe/analysis caps, and an explicit nested-protocol whitelist.

Primary local media, metadata probes, playlists, artwork, and standalone
subtitle inputs use no-follow regular-file validation and descriptor-backed
reads. FFmpeg receives custom AVIO for the primary local media file. Formats
that declare companion files internally, such as local HLS segments or
IDX+SUB, may still use FFmpeg's restricted local `file` protocol for those
companions; this is the one deliberate exception needed for those formats.

Embedded subtitles currently supported by Kitchensink are Text, HDMV text,
SRT/SubRip, SSA/ASS, DVD bitmap, DVB bitmap, and PGS. External subtitles need a
second FFmpeg demuxer and libass/bitmap renderer because a Kitchensink player is
bound to one `AVFormatContext`; that layer is deliberately separate from the
embedded-track implementation. The executable overrides Kitchensink's image
subtitle renderer with a hardened, symbol-compatible implementation that
validates rectangle geometry, palette count and indices, bounds cache growth,
uses the correct FFmpeg RGB32 palette layout, and repairs unsafe cleanup and
callback behavior in the upstream implementation.

## Library safety rules

- Open directories without following symlinks and inspect entries relative to
  the opened descriptor with `fstatat(AT_SYMLINK_NOFOLLOW)`.
- Capture the root device and do not cross `st_dev` during recursion.
- Abort the active scan on `EIO`, `ESTALE`, `EBADF`, or `EFAULT`; do not retry
  the same file descriptor.
- Bound recursion depth, item count, and metadata-probe time.
- Probe changed or previously unseen audio/video files only. Each library pass
  has a 512-file enrichment budget, a five-second per-file interrupt deadline,
  and stops enrichment after eight failures while preserving the basic index.
- Persist results transactionally; an incomplete, cancelled, visitor-stopped,
  or I/O-failed scan rolls back and keeps the last usable index.

The library joins resume data into each indexed row to provide Continue
Watching and Recently Played views. FFmpeg enrichment records title, duration,
resolution, container, and primary codecs without competing with playback: the
library scan thread is cancelled and joined before a movie starts.

Library search uses the PS5 SDL backend's SceImeDialog bridge. The backend
delivers the completed UTF-8 string as one or more `SDL_TEXTINPUT` events and a
Return event; cancel delivers no text event, so the UI also observes dialog
visibility to leave search mode cleanly. Search terms are normalized once,
matched as AND tokens without per-entry allocation, and the filtered/sorted
index is cached until the library generation, category, query, or sort changes.
Sort selection is stored in SQLite and item identity is preserved by path when
metadata refreshes or ordering changes.

Favorites use a separate path-keyed SQLite table so a favorite survives when a
removable USB drive is temporarily absent. Toggling updates the visible entry
immediately. If a scan transaction is active, the write is queued and committed
through the scanner's existing connection after the root transaction; pending
state is overlaid on refreshed rows so the UI never rolls back a recent toggle.

SQLite schema version 2 is migrated transactionally from earlier tables.
Future schema versions are rejected, failed migration rolls back, and database
text/row size is capped before values enter UI or playback state.

Artwork discovery remains outside the database to avoid a schema migration and
stale removable-drive paths. The main thread caches only the selected title's
texture. Every selection or completed rescan uses an immediate-directory,
non-symlink lookup; the loader then revalidates file identity/size, encoded
format, dimensions, and pixel count before invoking SDL_image. Only JPEG and PNG
decoder objects are pulled from the static SDL_image archive.

## Packaging

The supported release is one injectable payload:

```text
ps5-media-center-standalone.elf
```

At startup it writes only the small font, icon, and runtime manifest required
by the embedded player, repairs `/system_ex/app/PSMC00001`, and registers
`/user/app/PSMC00001/sce_sys`. It also removes the obsolete `PSMR00001`
registration created by alpha.11/alpha.12. The
player is launched from its compressed embedded bytes, so no separate player
ELF transfer or HBL installation is required. AppInst and kernel-system imports
belong to the outer resident launcher; the embedded player ELF retains its
narrower import boundary. See `docs/STANDALONE_LAUNCHER.md`.
