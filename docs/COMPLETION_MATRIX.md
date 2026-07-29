# Completion matrix

This matrix keeps offline evidence separate from runtime claims. Alpha.5 was
verified offline. Alpha.8 is now installed and substantially verified on PS5;
see [ALPHA8_HARDWARE_VERIFICATION.md](ALPHA8_HARDWARE_VERIFICATION.md).
Historical
offline evidence remains in [ALPHA5_VERIFICATION.md](ALPHA5_VERIFICATION.md)
and [FULL_VERIFICATION_AUDIT.md](FULL_VERIFICATION_AUDIT.md). The alpha.36
audio-clock, buffer, and DLNA/NAS changes have a separate
[offline verification record](ALPHA36_OFFLINE_VERIFICATION.md).

| Requirement | Current evidence | Status |
| --- | --- | --- |
| Native PS5 player package | Stripped Prospero DYN player embedded as verified gzip in one standalone Prospero DYN launcher payload with per-file hashes and notices | Offline verified |
| Broad container/codec support | Exact PacBrew archive symbol audit: 353 demuxers, 500 decoders, 42 protocols | Linked; per-format hardware ladder pending |
| Embedded subtitles and live switching | Kitchensink stream selection plus project-owned hardened text/bitmap rendering path; symbol override and link selection verified | Offline verified; runtime unobserved |
| External subtitles and timing | SRT/ASS/WebVTT plus bounded FFmpeg SUP/IDX+SUB layer, safe primary AVIO, transactional selection, delay persistence | Offline verified; runtime unobserved |
| Multiple audio/video tracks | Transactional live stream replacement, resource rollback, language fallback, persistent per-title preferences | Offline verified; runtime unobserved |
| VLC-like core controls | Pause, bounded seek, chapters, mute/volume, audio/video/subtitle cycles, OSD, three resize modes, all documented VLC Desktop aspect overrides, and all documented crop ratios | Source/build and exhaustive geometry tests verified; controller and sync unobserved |
| Organized media library | Existing explicit Movie/TV selection is hardware verified; alpha.12 removes automatic roots and starts at `/`; alpha.14 repairs removable-storage metadata fallback; alpha.15 makes whole-library classification cancellable and non-blocking with progress | Exact alpha.15 production discovery passed the real USB hardware probe; background UI, cancellation, and commit require controller acceptance |
| Network playback | Direct URL input plus cancellable DLNA/UPnP discovery and paged ContentDirectory browsing; bounded HTTP responses/results, strict URL policy, FFmpeg reconnect handling, and small decoded-frame queues | Host protocol tests, Linux socket/HTTP/SOAP integration tests, and target build verified; live PS5 NAS/network behavior unobserved |
| Playback rate and advanced video controls | Persistent Best fit, Fill screen, Fullscreen stretch, 9 aspect choices, and 11 crop choices are host-tested; Kitchensink exposes no playback-rate API and selectable deinterlace is not implemented | Implemented scope is offline verified |
| Generic playlists | Bounded M3U/PLS/XSPF parsing, relative-path resolution, credential/network filtering, visible errors, ordered queue | Host/target verified; runtime auto-advance unobserved |
| Artwork and richer metadata | Selected-title local JPEG/PNG sidecars and FFmpeg metadata probing use strict limits; alpha.12 adds cancellable interior-frame preview extraction when no cover is usable | Offline verified; generated preview runtime pending |
| Stable packaging choice | One resident payload embeds the compressed player, uses one Media-category `PSMC00001` registration for the tile/runtime host, migrates away the duplicate `PSMR00001` Games entry, and serves loopback-only `:9040/launch`; no HBL/websrv/8080/web-picker or pre-existing `/data/homebrew` dependency | Alpha.13 asset/tile installation and launch are hardware evidenced; final dashboard-category visual check remains |
| Direct local playback | Websrv passed the USB-root MKV as `argv[1]`; Matroska/HEVC/AAC player creation and advancing heartbeats succeeded | **Hardware verified for the tested file** |
| Arbitrary Movie/TV sources | Touchpad opened the picker and navigated to `/`; selected TV folder persisted as one two-episode show; Cross entered the episode view and played an episode; selected file persisted as one explicit Movie and launched from its final library row | **Hardware verified for the tested sources** |
| Runtime behavior | VideoOut, AudioOut, controller input, A/V sync, decode speed, memory use, DLNA interoperability, and filesystem behavior are format- and hardware-specific | Partial PS5 evidence only; the alpha.36 audio-clock and DLNA changes require the remaining hardware ladder |
| Offline completion | Clean tests/build, analyzer pass, exact imports, hardened I/O/state paths, corpus hashes, exact archive contents and hashes | **Required for alpha.5** |

Continue with [HARDWARE_TEST_LADDER.md](HARDWARE_TEST_LADDER.md) and keep
unobserved behaviors explicitly pending.
