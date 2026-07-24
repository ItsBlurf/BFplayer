# Hardware test ladder

Alpha.5 was verified offline only. Alpha.8 hardware verification now covers
touchpad browsing, USB-root discovery, Movie/TV-source classification, grouped
episode entry/playback, and local/direct playback; see
[ALPHA8_HARDWARE_VERIFICATION.md](ALPHA8_HARDWARE_VERIFICATION.md). Continue
from the first still-unverified behavior instead of repeating successful tests.

Run exactly one new variable per step and collect websrv/HBL stdout. Stop on a
console disconnect, black screen that does not recover, or a fatal filesystem
error.

1. Launch with no media argument; verify window creation and clean Options exit.
2. Play a local 720p H.264/AAC MP4 with no subtitles for ten minutes.
3. Pause/resume and seek repeatedly; verify A/V sync and clean return.
   Use R2+Triangle for every display-aspect override, L2+Triangle for every
   crop ratio, and L2+R2+Triangle for Best fit, Fill screen, and Fullscreen
   stretch on 4:3, anamorphic, 16:9, and ultrawide samples; verify persistence.
4. Play 1080p H.264 in MKV with two AAC audio tracks; switch tracks ten times.
5. Switch among embedded SRT, ASS, and Off.
6. Test embedded PGS separately and watch memory use.
7. Test sidecar SRT, ASS, WebVTT, SUP/PGS, and IDX+SUB. Adjust timing in both
   directions and verify the selected sidecar and delay survive reopening.
8. Reopen the multitrack sample and verify audio/subtitle preferences restore;
   remux it with different stream indices and verify language fallback.
9. Test USB and internal storage scans on small roots, then larger roots. Verify
   Continue Watching, Recently Played, title, duration, resolution, and codec
   metadata, then modify one file and confirm only that metadata is refreshed.
   Use Circle on an episode and verify Play From Here advances only after a
   natural ending; Options must stop the queue and return immediately. Press
   Touchpad and verify Add Media Source opens without an IME or black screen.
   Add one file as a Movie and one folder as a TV Show, then verify the show
   opens to a naturally ordered episode list. Verify R3 cycles all six
   sort modes, the selected path does not jump when ordering changes, and the
   chosen sort survives playback and relaunch. Toggle Favorites with L3 during
   an active scan, switch to the Favorites category with Left/Right, relaunch,
   and verify the choice persisted without pausing navigation. Play local M3U,
   PLS, and XSPF lists containing relative paths and confirm order, automatic
   advance, unsupported-entry filtering, and visible malformed-list errors.
   Add title-specific and folder-level JPEG/PNG artwork, confirm priority and
   aspect ratio, then replace an artwork file and verify Rescan refreshes it.
10. Use **Play a network URL** for HTTP/HLS, HTTPS, RTSP, RTMP, and UDP/RTP
    samples, including clean connection failure. Confirm a URL containing
    `user:password@` is rejected before launch and a signed query value never
    appears in stdout/stderr. Then test SMB only through websrv's HTTP proxy.
11. Measure 1080p HEVC/VP9, then 4K samples. Treat frame drops as a performance
    limit, not a reason to stack privileged/debug payloads.

Never mass-kill processes, restart the BigApp in a loop, or launch through a
background elfldr context. Older firmware must follow websrv's own BigApp
budget guard.
