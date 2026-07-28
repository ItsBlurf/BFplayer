# Changelog

## 0.1.0-alpha.32 - external and online subtitles

- Added an Options > Subtitles screen for embedded tracks, detected sidecars,
  manually browsed subtitle files, and downloaded subtitles
- Added a safe subtitle-only file browser that starts beside the playing video
- Added optional SubDL search and download using the user's own API key and
  preferred language list
- Download subtitles in the background, save them atomically under
  `/data/BFplayer/subtitles`, and select them without restarting the video
- Keep API keys out of URLs and logs; accept downloads only from SubDL's
  official HTTPS hosts

## 0.1.0-alpha.31 - responsive seek and track switching

- Wake the demuxer immediately for seeks instead of waiting for full buffers
- Keep the newest seek request when inputs arrive close together
- Refresh audio and subtitle packets around the current playback position
- Stop rebuilding the video texture when only the subtitle track changes

## 0.1.0-alpha.30 - low-memory standalone launch

- Stream the embedded compressed player to an atomically replaced runtime file
  with a fixed 64 KiB buffer instead of allocating the full decompressed image.
- Verify an existing runtime player by regular-file identity, size, ELF header,
  and gzip CRC32 before trusting it after reinjection.
- Launch the verified runtime image through a read-only private file mapping,
  removing the former 36 MB launch-time heap allocation and decompression pass.
- Retain singleton ownership and player locking around clean reinjection, and
  add diagnostics identifying cached, CRC-verified, streamed, and mapped launch
  paths.

## 0.1.0-alpha.29 - startup panic diagnostics

- Added durable startup-stage markers before diagnostics initialization and
  around SDL, SDL_kitchensink, window, renderer, and controller setup, allowing
  a launch-time crash or kernel panic to be narrowed to the last completed
  initialization boundary.
- Extended the log collector to produce `diagnostic-summary.txt` with marker
  counts, the last matching record, and bounded tails for every retrieved log.
- Added structural regression coverage for startup-stage ordering and collector
  coverage.

## 0.1.0-alpha.28 - complete internal BFplayer identity

- Renamed the remaining internal namespace, public header tree, build symbols,
  launcher symbols, tests, and test-corpus labels from the former project
  identity to `BFplayer`.
- Updated the repaired BigApp host content ID to use `BFPLAYER` while retaining
  title ID `PSMC00001`, which is required to update the existing Media tile.
- Retained legacy file/database recognition only as split migration literals,
  so existing installations are upgraded without exposing or reviving the old
  product identity.
- Added a repository-wide naming regression test.

## 0.1.0-alpha.27 - BFplayer naming completion

- Replaced the remaining legacy product-name menu, exit, deployment, and
  diagnostic labels with `BFplayer`.
- Renamed the host CMake project while preserving the existing title ID and
  internal compatibility identifiers required to update installed tiles.
- Prevent multiple player processes from initializing SDL/video memory at the
  same time: the launcher tracks its active child and the player holds a
  non-blocking cross-reinjection lock before any SDL initialization.
- Added static coverage preventing the old visible menu labels from returning.

## 0.1.0-alpha.26 - source aspect and safe playback geometry

- Read display-aspect metadata from the original demuxed stream so proportional
  decoder downscaling cannot alter the intended picture shape.
- Start every video in `Best fit`, `Original`, and `No crop`; manual geometry
  changes are playback-local and cannot distort the next video.
- Add source SAR and DAR values to diagnostics for hardware verification.

## 0.1.0-alpha.25 - VP9 threading and YUV color fix

- Enable frame and slice threading together when FFmpeg reports that a video
  decoder supports both, instead of discarding slice threading whenever frame
  threading is available.
- Log the threading modes that FFmpeg actually activated for each video.
- Map planar YUV420 output to SDL `IYUV`, matching FFmpeg's Y/U/V plane order,
  instead of `YV12`, whose Y/V/U order could swap chroma planes.

## 0.1.0-alpha.24 - BFplayer rename and 4K WebM optimization

- Renamed the product, runtime paths, logs, packages, and visible tile metadata
  to BFplayer while preserving title ID `PSMC00001` to update the existing tile.
- Preserve an existing library by atomically migrating the legacy database on
  first launch.
- Vendor SDL_kitchensink so decoder fixes are reproducible, enable its actual
  frame/slice threading checks, and use all 16 logical processors reported by
  SDL.
- Convert video larger than 1920x1080 to a proportional even-sized output,
  reducing 4K conversion and presentation cost without distorting its ratio.
- Restore the BFplayer logo in the library header and empty-library view.
- Show the complete library shortcut bar, including `Triangle Remove`.
- Use a five-second launch countdown, coalesce duplicate launch requests, and
  retain clean serialized reinjection.

## 0.1.0-alpha.23 - original aspect calculation fix

- Correctly convert SDL_kitchensink's sample/pixel aspect ratio into the final
  display aspect ratio using `frame width / frame height * SAR`.
- Keep ordinary square-pixel videos at their encoded frame ratio instead of
  incorrectly treating `SAR 1:1` as a square display.
- Add regression coverage for 16:9 square-pixel, 4:3 fallback, and anamorphic
  720x480-to-16:9 video.

## 0.1.0-alpha.22 - WebM decode diagnostics test build

- Explicitly use eight FFmpeg decoder threads instead of depending on runtime
  CPU autodetection, which can otherwise leave 4K VP9 decoding single-threaded.
- Enlarge the video packet queue from 16 to 64 and decoded-frame queue from 2
  to 3 to better absorb removable-storage read jitter.
- Log source codec, profile, pixel format, bit depth, resolution, frame rate,
  bitrate, actual decoder thread counts, queue fill, and delivered-frame rate.
- Warn when a 4K high-frame-rate or 10-bit VP9/AV1/HEVC source is likely to
  exceed the current software decoder's real-time capacity.

## 0.1.0-alpha.21 - Circle input safety test build

- Stop Circle from launching the selected video or playback queue at the
  library root.
- Keep Circle exclusively as Back while browsing a TV show or season, so a
  second press after returning to the root cannot unexpectedly start media.
- Correct the in-app controls screen and README to match the input behavior.

## 0.1.0-alpha.20 - original aspect ratio test build

- Start every video in `Original` aspect mode, honoring the stream display
  ratio and falling back to decoded frame dimensions when metadata is absent.
- Keep manual aspect overrides local to the current playback so an override
  cannot distort a different video opened later.
- Label the automatic mode and playback-setting scope clearly in the UI.

## 0.1.0-beta.1 - first public beta

- Keep the active sort mode in the library status line and simplify the footer
  shortcut to `R3 Sort`.
- Rewrite the project README for the first public beta.
- Carry forward the alpha.19 TV-show sorting and clean reinjection fixes.

## 0.1.0-alpha.19 - library sort and clean reinjection test build

### Library sort

- Sort TV shows by their displayed show names instead of the filename or title
  of whichever episode represents each collapsed show row.
- Preserve the selected TV show when changing sort modes, even when the
  representative episode changes under Recent, Newest, Duration, or Size.
- Show the active shortcut and mode in the main library footer as
  `R3 Sort: <mode>`.

### Clean reinjection and update

- Add a process-held exclusive launcher lock so repeated or concurrent
  injections serialize instead of updating files or registrations twice.
- Gracefully stop the previous resident launcher before applying the new
  build's atomic runtime updates, then bind the replacement service only after
  the update finishes.
- Compare managed runtime and tile files before writing, atomically replacing
  only files whose bytes changed and aborting on filesystem read errors.
- Keep the same `PSMC00001` title registration, library database, settings,
  and media sources while replacing the resident launcher with the new build.
- Record completion of the obsolete `PSMR00001` migration cleanup so normal
  reinjections do not repeatedly uninstall an already removed legacy title.
- Add takeover diagnostics and structural regression checks for update order.

## 0.1.0-alpha.18 - native lifecycle test build

### Launch handoff

- Retry all browser-supported close and history-return methods after the
  shortened launch handoff instead of making only one close attempt.
- Replace the PS-button fallback with `Press O to close this window`, because
  Circle dismisses the browser without opening the PlayStation control center.
- Send a native PS5 notification after injection finishes installing the
  runtime assets, Media tile, and loopback launch service.

### Native exit

- On a normal BFplayer exit, close SDL/player resources and ask
  SystemService to terminate the active `PSMC00001` BigApp host so the console
  returns to PlayStation Home instead of leaving a black VideoOut surface.
  The title ID is verified before termination so a directly injected player
  cannot close an unrelated game.
- Add lifecycle diagnostics for the detected BigApp ID and any failed
  SystemService termination request.
- Extend the standalone structural tests to require the notification,
  automatic-close retry, Circle fallback, and native BigApp exit paths.

## 0.1.0-alpha.17 - playback-resume and season-navigation test build

### Playback corrections

- Apply saved resume positions only after SDL_kitchensink enters its playing
  state; the previous stopped-state seek could be ignored even though the
  database record loaded correctly.
- Save the final resume record from the playback clock captured before decoder
  shutdown, with explicit diagnostics for missing, skipped, applied, and
  failed resume attempts.
- Route embedded subtitle rendering through SDL_kitchensink's existing
  explicit synchronization timestamp so the signed subtitle timing value now
  affects embedded text tracks as well as external subtitle sidecars.
- Clarify subtitle timing direction in the player: a positive value displays
  subtitles later, and Cross resets the value to zero.
- Rename the unclear On-screen display setting to Pop-up message duration and
  explain that it controls temporary seek, pause, volume, and status messages.

### TV-library hierarchy

- Group every first-level folder below a configured TV-show root as a season,
  including episodes stored in deeper folders such as
  `Season 1/Disc 1/Episode.mkv`.
- Add Show -> Season -> Episode controller navigation with natural season
  ordering, episode counts, correct back navigation, and season-scoped queues.
- Keep media files directly inside the show folder playable alongside its
  season folders instead of hiding or incorrectly grouping specials.
- Add host regression coverage for nested seasons, direct episodes, sibling
  path boundaries, and Windows-path normalization.

## 0.1.0-alpha.16 - full offline-audit test build

### Add Media reliability

- Made first-run Cross open Add Media even while the initial library worker is
  settling, removing a short interval where the focused action could appear
  unresponsive.
- Added a clear in-screen action legend for opening one movie, adding one TV
  folder, or explicitly importing a mixed library; long instructions are
  width-checked and no longer clip in the artwork panel.
- Added correct keyboard routing for the Add Media browser instead of letting
  keyboard events fall through to the hidden library selection.
- Prevented movie/TV-source mutations from racing a running or not-yet-applied
  bulk import. Import cancellation still discards every partial result.
- Restored background scanning after every failed add, import-save, or removal
  path, and after a successful source removal.
- Reports a source-removal rollback-save failure instead of silently ignoring
  a second database error.
- Expanded root/source-browser logs with shown, visited, directory, media,
  fallback, unreadable, symlink, filtered, and non-regular counts.
- Preserved fatal-I/O precedence over simultaneous cancellation so a storage
  failure cannot be mislabeled as a harmless user cancel.

### Player and settings audit

- Made multi-field playback settings writes atomic; a failed database write
  now rolls back every field and the UI restores the previous live values.
- Added a tested rollback case for a settings batch that fails after an
  earlier value was written.
- Added PS5-reachable mute/unmute to the playback Options menu and removed
  inaccurate Create-button claims.
- Corrected the complete in-app shortcut page so Square is explicitly scoped
  to Add Media rather than the normal library.
- Stopped static Controls/About pages from drawing a misleading selected row.
- Pixel-bounded playback messages and modal text so long decoder/file errors
  cannot overlap the timestamp or render off-screen.
- Final resume, track-preference, and playback-setting save failures now
  produce explicit diagnostics.
- Reset held-button state on controller hot-plug and keep the main controller
  handle synchronized while the library is active.
- Made launcher error formatting bounded instead of concatenating into a fixed
  buffer.

### Offline verification

- Added and visually inspected a deterministic Add Media preview alongside the
  empty-library preview.
- Rechecked the current SDK v0.41/PacBrew v0.37 inputs, host unit suite, GCC
  analyzer, PS5 compile/link/package, standalone ELF structure, embedded
  English-only Media metadata, secret scan, and release contents.

## 0.1.0-alpha.15 - responsive import test build

### Non-blocking library import

- Moved whole-library discovery to a dedicated cancellable worker so large
  folders no longer freeze controller navigation or rendering.
- Added live checked-entry, Movie, TV Show, and current-path progress to Add
  Media.
- Pressing Square during discovery now requests cancellation; canceled partial
  results are discarded and never mistaken for a completed import.
- Preserved fatal-I/O aborts, traversal budgets, no-follow behavior, and the
  removable-storage `lstat` fallback in the worker path.

### First-run experience

- Made the empty library a full-width focused start screen instead of showing
  an unused artwork panel.
- Cross now opens Add Media directly whenever the current library view is
  genuinely empty, with a matching focused call to action and footer. Empty
  searches and categories now explain how to recover instead of pretending the
  whole library is empty.
- Added Search/Clear Search to the controller Options menu so the PS5 IME and
  every library workflow are reachable without a keyboard.
- Added Playback Settings to the in-player Options menu, with live volume,
  seek-step, OSD, scaling, aspect, and crop changes plus the same persisted
  resume/subtitle defaults available from the library.
- Made Left and Right move backward and forward through scaling, aspect, and
  crop values instead of both directions advancing.

## 0.1.0-alpha.14 - import and complete-player test build

### Add Media repair

- Fixed bulk import on PS5 removable/exFAT storage by falling back from
  descriptor-relative stat to a no-follow path stat when the mount exposes an
  entry but rejects `fstatat`.
- Made Square import the highlighted directory, or the current directory when
  a file is highlighted, with contextual footer text.
- Added explicit import-empty/failure diagnostics and a dedicated visible
  notification instead of hiding the result at the clipped end of a status
  line.

### Complete playback controls

- Replaced immediate Options-to-exit with an in-player menu for Resume,
  Controls, Subtitle timing, and Return to library.
- Added complete in-app library and playback control references.
- Restored D-pad to predictable seeking at all times; subtitle timing is now a
  visible playback-menu setting instead of a hidden modal control takeover.
- Added persistent settings for default volume, short and long seek steps, OSD
  duration, resume behavior, and automatic subtitle selection.

### Interface audit

- Removed repeated old-logo rendering and decorative fake artwork from the
  runtime library and preview.
- Simplified the main library footer and promoted advanced actions to the
  Options/Controls pages.
- Added a documented product/UX and current build-input audit.

### Test-build replacement

- Added a loopback-only graceful shutdown/takeover route so a newer standalone
  payload can replace an already-resident alpha.14-or-newer launcher instead
  of failing to bind port 9040. The first upgrade from alpha.13 still requires
  ending that older launcher once because it does not implement the route.

## 0.1.0-alpha.13 - single-tile test build

### Dashboard registration

- Consolidated the visible Media tile and BigApp runtime host onto the single
  `PSMC00001` title ID.
- Changed the repaired `/system_ex/app/PSMC00001` host metadata to the
  Prospero Native Media App category.
- Removed the separate `PSMR00001` native-Game registration that caused a
  duplicate BFplayer entry in Games.
- Added a startup migration that uninstalls the legacy `PSMR00001`
  registration and removes its old `/user/app` metadata before registering the
  single Media entry.

### Add Media reliability

- Fixed the `/` source browser so PS5 mount points such as `/data`, `/user`,
  and attached storage remain visible and can be entered explicitly.
- Kept the picker descriptor-bound and symlink-safe, with PS5 removable-drive
  `lstat` fallback, bounded listings, fatal-I/O aborts, and visible errors.
- Added a combined traversal budget to bulk library import so many child
  folders cannot bypass the per-folder limits or silently return partial
  results.
- Roll back in-memory source changes when database persistence fails.

## 0.1.0-alpha.12 - manual-library test build

### Manual library management

- Removed startup storage discovery and the Rescan action. Only media sources
  explicitly selected by the user are indexed and retained in the library.
- Add Media now opens at the PS5 filesystem root (`/`).
- Added a two-press Triangle confirmation to remove the selected movie or TV
  show source and its cached rows from the library.
- Added an explicit bulk import action for a chosen folder: loose video files
  become Movies, while immediate child folders containing video recursively
  become TV Shows.
- Kept recursive source traversal descriptor-bound, symlink-safe,
  same-filesystem-only, bounded, cancellable, and fatal-I/O aware.

### Artwork and launch handoff

- Added bounded background FFmpeg extraction of an interior video frame when
  no valid local JPEG/PNG artwork is available, including alternate positions
  to avoid mostly black frames.
- Removed the old logo from the empty artwork panel.
- Replaced the white loopback launch response with a dark three-second
  countdown page that attempts to close or return automatically.
- Preserved `PSMC00001` as an English-only Media-category dashboard tile.

## 0.1.0-alpha.11 — independent runtime-host test build

### HBL-independent startup

- Replaced HBL's shared `FAKE00000` dependency with the project-owned
  `PSMR00001` BigApp host.
- Added automatic AppInst registration plus `/system_ex` host creation and
  repair before the dashboard tile is published.
- Rewrites missing, zero-length, partial, symlinked, or invalid host artifacts
  safely instead of treating any existing path as usable.
- Recreates all required `/data/homebrew/BFplayer` runtime assets; the
  folder is not required to remain on the console.
- Uses the BFplayer icon and title for the active BigApp host so the PS
  switcher can identify the running app correctly.

### Payload size and integrity

- Strips the player ELF and embeds it as gzip instead of embedding the raw
  link output.
- Expands the player in memory on demand, validates the exact decompressed size
  and ELF64 header, and logs compression/launch diagnostics.

### Interface

- Replaced the procedural in-app play/blade mark with the exact dashboard-tile
  artwork.
- Replaced footer button names with rendered PlayStation-style Cross, Circle,
  Square, Triangle, D-pad, touchpad, stick, and Options prompts.

## 0.1.0-alpha.10 — standalone test build

### Self-contained dashboard launch

- Replaced the websrv/port-8080 launch path with one resident
  `bfplayer-standalone.elf`.
- Embedded the complete player ELF, Noto Sans runtime font, icon, tile data,
  AppInst setup, and minimal BigApp/ELF-loader core into that single payload.
- Added a loopback-only listener on `127.0.0.1:9040` with exactly one
  `GET /launch` route.
- Removed the separate tile-installer ELF, `homebrew.js`, websrv package, HBL
  catalog path, and file-picker dependency.
- Added standalone launcher/player logs and an embedded runtime manifest.
- Added GPL-3.0-or-later source attribution and license text for the retained
  upstream BigApp loader core.

## 0.1.0-alpha.9 — offline test build

### Display controls

- Added all VLC Desktop display-aspect choices: Default, 1:1, 4:3, 16:9,
  16:10, 2.21:1, 2.35:1, 2.39:1, and 5:4.
- Added all VLC Desktop crop choices: Default, 16:10, 16:9, 1.85:1, 2.21:1,
  2.35:1, 2.39:1, 5:3, 4:3, 5:4, and 1:1.
- Kept source crop and display-aspect override independent, matching VLC
  semantics.
- Added Best fit, Fill screen (crop), and Fullscreen stretch resize modes.
- Added independent persistent SQLite settings and diagnostic fields for scale,
  aspect, and crop.
- Added exhaustive host geometry tests, including anamorphic pixel-aspect
  correction.

### Dashboard launch

- Added a `PSMC00001` Media-category dashboard tile.
- Added a separate `bfplayer-tile-installer.elf` so AppInst/kernel-system
  privileges do not enter the player ELF.
- The tile calls websrv's `/hbldr` route with the exact BFplayer ELF and
  working directory, bypassing the catalog, `homebrew.js`, and file picker.
- Added a primary direct-tile bundle, exact SHA-256 manifests, and install
  documentation.
- Kept the unsafe compatibility-FPKG/OpenOrbis experiments out of the package.

### Visual redesign

- Replaced the glossy generic icon with a custom BFpilot-inspired pixel-art
  blade piercing a gold play symbol.
- Rebuilt the library around a compact branded header, status pill, bordered
  content panels, blue/gold selection treatment, real empty-library guidance,
  improved artwork empty state, and a smaller control rail.
- Added a deterministic 1920×1080 UI preview renderer and checked-in preview.

### Verification scope

- Built with the PS5 SDK wrappers as a FreeBSD-ABI x86-64 DYN payload.
- Passed all host tests and PS5-target compile/link checks.
- Passed SDK dependency checks for both player and tile installer.
- No console connection, deployment, or PS5 runtime test was performed for this
  build, per owner instruction.
