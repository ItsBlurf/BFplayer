# Alpha.8 hardware verification

## Installed build

- Version: `0.1.0-alpha.8`
- ELF SHA-256:
  `d7d1ef98decf7eb2cf840cca3cc5cc06900b8989938eb7f0ce069546b5419413`
- Installed and local manifest hashes matched.
- PS5 DYN/FreeBSD/x86-64 inspection passed.
- All ten host tests passed.
- All ten linked PS5 dependencies were available.

## Requirement evidence

| Requirement | PS5 evidence |
| --- | --- |
| Touchpad must not black-screen | PacBrew's PS5 SDL maps physical Touchpad to SDL Start and physical Options to SDL Back. Alpha.8 applies this mapping centrally. A physical touchpad press opened `/mnt/usb0`; the picker then navigated through `/mnt` to `/` without exiting or crashing. |
| Browse anywhere | The native picker opened USB directories and successfully reached the filesystem root. It uses `lstat`, ignores symlinks, and does not cross device boundaries during recursive scans. |
| USB-root media | `/mnt/usb0` completed with 15 entries, one media item, zero recoverable errors, and zero fatal errors. |
| Folder becomes one TV Show | `/data/media/PS5MC-Test` was accepted as a TV-folder source. The scan published `shows=1 episodes=2 explicit_movies=0 total=3`. |
| Show opens to episodes | The first Cross entered the grouped show without a play request. Navigation stayed in the episode view; the next Cross played `/data/media/PS5MC-Test/ffmpeg-pgs-supsample.mkv`. Returning from playback preserved `shows=1 episodes=2`. |
| Selected file remains one Movie | The USB MKV was accepted as a movie-file source. The next scan published `shows=1 episodes=2 explicit_movies=1 total=3`, proving it was not absorbed into the TV group. |
| Explicit Movie row plays | Cross on the final explicit One Pace Movie row issued a one-item request for that exact USB path. Matroska/HEVC/AAC initialization succeeded and playback heartbeats advanced. |
| Direct HBL playback | The USB MKV opened as Matroska with HEVC 1920x1080 video and AAC 48 kHz stereo. Player creation and advancing heartbeats succeeded; the former unsupported float-sample error did not recur. |
| Physical Options behavior | In playback, physical Options produced `playback-stop requested=options`; in the library it produced a clean `application-end result=0`. |

## Logs

- Touchpad and root navigation:
  `diagnostics/session-20260723-234940/latest.log`
- TV-folder source classification:
  `diagnostics/session-20260723-235448/latest.log`
- Group open, episode playback, and return:
  `diagnostics/session-20260723-235533/latest.log`
- Explicit Movie classification:
  `diagnostics/session-20260723-235641/latest.log`
- Explicit Movie row playback:
  `diagnostics/session-20260723-235914/latest.log`
- Earlier USB/direct-player evidence:
  `diagnostics/session-20260723-233538/latest.log` and
  `diagnostics/session-20260723-233647/latest.log`

All behaviors requested for Movie/TV source selection, USB-root discovery,
touchpad browsing, grouped episode entry/playback, and direct HBL playback have
now been exercised on the PS5.
