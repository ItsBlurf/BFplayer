# Hardware test ladder

Alpha.5 was verified offline only. Alpha.8 hardware verification now covers
touchpad browsing, USB-root discovery, Movie/TV-source classification, grouped
episode entry/playback, and local/direct playback; see
[ALPHA8_HARDWARE_VERIFICATION.md](ALPHA8_HARDWARE_VERIFICATION.md). Continue
from the first still-unverified behavior instead of repeating successful tests.

Run exactly one new variable per step and collect the standalone launcher and
player logs. Stop on a console disconnect, black screen that does not recover,
or a fatal filesystem error.

1. With HBL uninstalled and `/data/homebrew/PS5-MediaCenter` absent, inject only
   the alpha.15 standalone payload. Verify it removes the obsolete Games
   entry, recreates its files, leaves exactly one dashboard tile in Media, and
   shows the
   Media Center icon in the active-app switcher, launches with no media
   argument, and exits cleanly through the Options menu. Verify the dark launch countdown
   returns/closes, or follow its PS-button fallback.
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
9. Verify an empty library performs no automatic storage scan. Open Add Media
   and confirm it starts at `/`. Add small USB/internal sources, then larger
   sources. Verify
   Continue Watching, Recently Played, title, duration, resolution, and codec
   metadata, then modify one file and confirm only that metadata is refreshed.
   Use Circle on an episode and verify Play From Here advances only after a
   natural ending; Options then Return to library must stop the queue. Press
   Touchpad and verify Add Media Source opens without an IME or black screen.
   Add one file as a Movie and one folder as a TV Show, then verify the show
   opens to a naturally ordered episode list. Verify R3 cycles all six
   sort modes, the selected path does not jump when ordering changes, and the
   chosen sort survives playback and relaunch. Toggle Favorites with L3 during
   an active scan, switch to the Favorites category with Left/Right, relaunch,
   and verify the choice persisted without pausing navigation. Play local M3U,
   PLS, and XSPF lists containing relative paths and confirm order, automatic
   advance, unsupported-entry filtering, and visible malformed-list errors.
   Use Square on a highlighted chosen folder and verify loose videos become
   Movies while child folders containing videos become TV Shows. Confirm
   navigation and rendering remain responsive while the progress counters
   advance. Start another import and press Square again; confirm it reports
   cancellation and adds no partial sources. Then run the import to completion.
   Confirm
   Square is rejected at `/`. Press Triangle twice to remove a Movie and a TV Show and confirm
   only their library rows disappear. Add title-specific and folder-level
   JPEG/PNG artwork and confirm priority/aspect ratio. For a title without
   artwork, verify an interior video-frame preview appears and the old logo
   never appears in the right panel.
10. Use local playlists containing HTTP/HLS, HTTPS, RTSP, RTMP, and UDP/RTP
    samples, including clean connection failure. Confirm a URL containing
    `user:password@` is rejected and a signed query value never appears in
    logs. Direct SMB is outside the standalone build's supported protocols.
11. Measure 1080p HEVC/VP9, then 4K samples. Treat frame drops as a performance
    limit, not a reason to stack privileged/debug payloads.

Never mass-kill processes or restart the BigApp in a loop. Inject only the
standalone launcher for this test and do not stack websrv or an older Media
Center launcher.
