# Linked FFmpeg symbol audit

This audit records what is actually present in the PacBrew v0.37 PS5 static
archives used by this project. It is stronger evidence than a desktop
`ffmpeg -formats` result, but it is still capability evidence rather than a PS5
performance guarantee.

## Result

| Archive | Evidence | Count |
| --- | --- | ---: |
| `libavformat.a` | unique `ff_*_demuxer` definitions | 353 |
| `libavcodec.a` | unique `ff_*_decoder` definitions | 500 |
| `libavformat.a` | unique `ff_*_protocol` definitions | 42 |

The 42 compiled URL protocols are:

```text
async, cache, concat, concatf, crypto, data, fd, ffrtmpcrypt,
ffrtmphttp, file, ftp, gopher, gophers, hls, http, httpproxy,
https, icecast, ipfs_gateway, ipns_gateway, md5, mmsh, mmst, pipe,
prompeg, rtmp, rtmpe, rtmps, rtmpt, rtmpte, rtmpts, rtp,
rtp_parse_set_dynamic, sctp, srtp, subfile, tcp, tee, tls, udp,
udplite, unix
```

Separate demuxer definitions confirm HLS, live FLV, RTP, RTSP, SAP, and SDP.
Representative container demuxers confirmed by the same audit include
Matroska/WebM, MOV/MP4/3GP, AVI, MPEG-TS, MPEG-PS, HLS, FLV, ASF, Ogg, Real,
WTV, IVF, NUT, Bink, Smacker, SUP, and VobSub.

Representative decoder definitions include H.264, HEVC, AV1, VP8, VP9, VVC,
MPEG-2, MPEG-4 Part 2, VC-1, ProRes, DNxHD/DNxHR, AAC, AC-3, E-AC-3, DTS,
TrueHD, FLAC, Opus, Vorbis, ASS, PGS, DVD, and DVB subtitles.

## Reproduction

Load `ps5dev-env.ps1`, locate PacBrew beside the SDK, then enumerate **defined**
symbols with LLVM `nm` and count unique names matching these suffixes:

```text
ff_*_demuxer
ff_*_decoder
ff_*_protocol
```

Audited inputs:

```text
<toolchains>/pacbrew-v0.37/homebrew/lib/libavformat.a
<toolchains>/pacbrew-v0.37/homebrew/lib/libavcodec.a
```

## Research cross-checks

- [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
  contains the PS5 FFmpeg, SDL2, SDL2_ttf, and SDL2_kitchensink package recipes.
- [ps5-payload-dev/websrv](https://github.com/ps5-payload-dev/websrv) provides
  the BigApp homebrew-launch path, file picker, SMB HTTP proxy, and FFplay
  comparison implementation used by the package contract.
- [FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg) is the upstream reference
  for libavformat, libavcodec, libswresample, and libswscale behavior.

The local PacBrew archives remain authoritative for what this exact ELF links.
