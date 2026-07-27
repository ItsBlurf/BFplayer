# Deterministic test corpus

Generated files are staged in:

```text
payloads/test/bfplayer/
```

The MKV is 10.021 seconds at 1280x720/30 fps and contains:

- H.264 video
- AAC audio track 1: `eng`, `440 Hz English`
- AAC audio track 2: `jpn`, `880 Hz Alternate`
- embedded English SubRip
- embedded Spanish SubRip
- embedded styled ASS
- three named chapters at 0, 4, and 7 seconds

Matching external English SRT and styled ASS sidecars test automatic sidecar
discovery, external switching, libass rendering, and subtitle delay.

`ps5mc-multitrack-test.bitmap.sup` is an extracted PGS stream from FFmpeg's
public `supsample.mkv` sample and exercises the external palette/bitmap path.
`ffmpeg-pgs-supsample.mkv` retains the same PGS stream embedded beside H.264
video and exercises Kitchensink's embedded bitmap-subtitle path.

The staging script also uploads the project icon as
`ps5mc-multitrack-test.png`. Its exact-title sidecar name exercises local
artwork discovery, bounded PNG header validation, and aspect-correct rendering.

SHA-256:

```text
f8d4d77521274aaa5b6603e7cb740e644eb7ea85b3f08132197730484978e796  ps5mc-multitrack-test.en.srt
bcdd343024479a1ab631c1385363a9b17b00ede4bd70d499150745f5c76983de  ps5mc-multitrack-test.mkv
f6340733393d3e222f50eaa3c947597c980b9c3669d4f0e6f5d166fca302cf3e  ps5mc-multitrack-test.styled.ass
f03e72bbd34177046be57da8d760718a078d139126d073e9e28febdd9624b7d9  ps5mc-multitrack-test.bitmap.sup
e6c8f93f57d0371603704d7e7b16933e6c4c5df669da42b42a2a84de881e0f27  ffmpeg-pgs-supsample.mkv
2b5f1f1c9e21ef5ec8170bd4a081f69623b39fe439fe834b9bde1f4a5a32d5ae  ps5mc-multitrack-test.png
```

Expected controller test:

1. Cross pauses and resumes while the OSD remains visible.
2. Square alternates the audible 440 Hz and 880 Hz tracks and names them.
3. Circle cycles three embedded tracks, two sidecars, Off, then back to the
   first embedded track.
4. L1/R1 seeks among the three named chapters.
5. Touchpad enters delay mode; D-pad changes external subtitle timing.
6. The library displays the exact-title PNG in the selected-title artwork panel.
7. R2+Triangle cycles exact display-aspect overrides, L2+Triangle cycles exact
   crop ratios, and L2+R2+Triangle cycles resize modes; all three persist.
8. Options returns to the library without restarting or killing another
   process.
