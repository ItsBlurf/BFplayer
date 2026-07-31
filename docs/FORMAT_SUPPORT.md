# Format and capability matrix

This matrix describes code present in the linked PacBrew v0.37 FFmpeg 7.0.1
archives. It is not a performance guarantee. The current open PS5 stack has no
Sony hardware-video decoder integration, so all codecs below decode on CPU.

## Containers and sources

Confirmed demuxer/protocol symbols in the actual static archive:

| Group | Included |
| --- | --- |
| General containers | Matroska/MKV/WebM, MP4/MOV/3GP, AVI, MPEG-TS/M2TS, MPEG-PS/VOB, FLV, ASF/WMV, Ogg |
| Streaming | HLS, HTTP, HTTPS/TLS, FTP, RTSP, RTMP family, RTP/SRTP, MMST/MMSh, TCP/UDP, SCTP |
| Subtitle files | ASS/SSA, SRT/SubRip, WebVTT, MicroDVD, SubViewer |
| Local storage | Safe custom AVIO for primary files on `/data`, `/mnt/usbN`, `/mnt/extN`; restricted FFmpeg file access only for format-declared companions |
| Network libraries | DLNA/UPnP MediaServer discovery and ContentDirectory browsing; direct SMB is not linked |
| Direct network sources | HTTP(S), FTP, HLS, RTSP, RTMP family, RTP/SRTP, MMST/MMSh, TCP/UDP, SCTP |

The main menu can browse compatible NAS and LAN libraries through DLNA/UPnP,
or open a direct supported URL. DLNA discovery is limited to the local
multicast network and the player still treats every returned resource as an
untrusted bounded network input. A NAS without DLNA can expose HTTP(S), FTP,
or another listed stream protocol.

Network entries inside local M3U, PLS, and XSPF playlists accept only the
schemes confirmed in the linked archive and useful for media input:
HTTP(S), FTP, RTSP, RTMP/RTMPE/RTMPS/RTMPT variants, RTP/SRTP, TCP, UDP,
UDPLite, SCTP, MMST, and MMSh. HLS uses an HTTP(S) URL. Direct SMB is rejected.

Credentials in URL authority fields are rejected by the player. Query or
fragment-bearing URLs can still be played (for signed stream URLs), but the
player redacts those values from logs and does not store that complete URI in
the resume/preferences database.

Playlist extensions M3U/M3U8, PLS, and XSPF are indexed. The library expands
generic M3U, PLS, and XSPF files into bounded, ordered queues, resolves relative
paths beside the playlist, rejects symlink playlist files, and filters direct
network entries through the same protocol allowlist. HLS M3U8 is deliberately
passed intact to FFmpeg rather than expanded into media segments. End-to-end
playlist playback still needs hardware validation.

## Local library artwork

The library displays bounded JPEG or PNG artwork for the selected local item.
It searches only the media file's immediate directory, ignores symlinks/reparse
points, and chooses the first applicable convention in this order:

1. `<media-title>.jpg`, `.jpeg`, or `.png`
2. `<media-title>-poster.*`, `.poster.*`, `_poster.*`, or ` poster.*`
3. `poster.*`, `cover.*`, `folder.*`, then `front.*`

Encoded files are limited to 16 MiB, either dimension to 8192 pixels, and the
decoded image to 20 million pixels. JPEG/PNG dimensions are validated before
SDL_image decodes the file. Network items never trigger local sidecar scans.
Artwork is presentation-only and is not stored in SQLite; changing a file and
rescanning refreshes the selected texture.

See [FFMPEG_SYMBOL_AUDIT.md](FFMPEG_SYMBOL_AUDIT.md) for the reproducible
archive-symbol counts and complete protocol list.

The library extension registry was cross-checked against symbols in the exact
PacBrew v0.37 `libavformat.a` and `libavcodec.a`, rather than inferred from a
desktop FFmpeg build. It additionally indexes confirmed less-common inputs such
as raw AV1/VVC/VC-1, IVF, NUT, WTV, Bink/Smacker, R3D, APNG/animated GIF,
TrueHD/MLP, HCA, QOA, Musepack, Shorten, and SCC/MCC/STL subtitles. Indexing
means FFmpeg can be asked to open the file; console performance and individual
damaged or encrypted variants remain subject to the hardware ladder.

## Video decoders

The archive exports decoders for:

- H.264/AVC
- H.265/HEVC
- AV1
- VP8 and VP9
- MPEG-1/2 and MPEG-4 Part 2
- VC-1/WMV
- Theora
- MJPEG
- ProRes
- DNxHD/DNxHR

H.264 720p is the first acceptance target, followed by H.264 1080p. HEVC,
VP9, AV1, 4K, 10-bit, and high-bitrate files are experimental until measured
on the console. Unsupported speed is reported as a limit; the player does not
silently claim hardware acceleration.

The alpha.44 PS5 test corpus includes a 3840x2160 59.94 fps 10-bit VP9
Profile 2 WebM that reaches its source frame rate through the native HDR
presenter. That result applies to the measured file, not every VP9 or 4K
encode. An accurately converted 4K60 Main 10 HEVC HLG probe was
software-decoder-limited below 60 fps even though its HDR presentation stayed
inside the frame budget.

## Audio decoders

Confirmed decoders include AAC, AC-3, E-AC-3, ALAC, MP2/MP3, FLAC, Opus,
Vorbis, DTS/DCA, and TrueHD. FFmpeg downmixes output to 48 kHz stereo signed
16-bit PCM for the current PS5 SDL AudioOut backend. Bitstream passthrough and
multichannel AudioOut are not implemented.

## Video presentation

Playback preserves the stream display aspect ratio by default. Holding R2 and
pressing Triangle cycles three playback-local renderer modes: Fit
(letterbox/pillarbox), Fill (center crop without distortion), and Stretch.
Anamorphic pixel-aspect correction from Kitchensink is included in Fit/Fill
math. These are renderer controls; playback-rate changes, pan/scan positioning,
and a user-selectable deinterlace filter are not implemented.

BT.2020/PQ 10-bit video uses native PS5 HDR10 output without tone mapping to
SDR. BT.2020/HLG is converted to PQ for the same HDR10 output. The player
preserves source resolution by default; proportional 1080p conversion is an
explicit compatibility policy rather than an automatic response to 4K50/60.

## Subtitles

| Capability | State |
| --- | --- |
| Embedded SRT/SubRip, SSA/ASS, text | Implemented |
| Embedded DVD, DVB, PGS bitmap | Implemented with the project-owned hardened Kitchensink renderer override; offline build/static checks pass |
| Embedded font attachments | Handled by Kitchensink/libass |
| Live embedded track switching/Off | Implemented |
| External SRT, SSA/ASS, WebVTT and FFmpeg text formats | Implemented with independent demuxer + libass |
| Multiple matching sidecars | Implemented, natural-order cycle |
| Browse any local subtitle while playing | Implemented through Options > Subtitles |
| Online subtitle search/download | Optional SubDL API integration; user API key and provider quotas apply |
| External subtitle timing | Implemented, -10 to +10 seconds |
| External SUP/PGS or IDX+SUB bitmap | Implemented with palette/index/geometry validation, a 30-second decode deadline, packet/event limits, and a bounded 256 MiB decoded-cue cache |

## Explicit non-goals/limits for the first hardware build

- DRM-protected media
- Blu-ray/DVD menus and Java navigation
- Sony SceAvPlayer hardware decoding (no public open-SDK header/stub exists in
  this workspace)
- HDMI audio passthrough
- Guaranteed 4K HEVC/AV1 performance
- Panic-prone compatibility FPKG packaging
- Online metadata or artwork scraping
