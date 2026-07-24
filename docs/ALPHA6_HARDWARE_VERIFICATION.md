# Alpha.6 hardware verification

## Tested build

- Version: `0.1.0-alpha.6`
- PS5 ELF SHA-256:
  `1f63d6311145c8e1f1ffd7553d2b4e6352458ca149588f568f61a57e4e5702e4`
- Package: websrv/HBL BigApp at
  `/data/homebrew/PS5-MediaCenter`
- Installed `build-manifest.json` hash field matched the local package.

The target ELF passed architecture and dependency inspection. All ten host
tests passed after the scanner fallback change.

## Observed on PS5

### USB-root scan

The app scanned `/mnt/usb0` completely:

```text
library-scan end root=/mnt/usb0 complete=1 entries=15 media=1
recoverable=0 first_errno=0 skipped_symlinks=0 skipped_devices=0
```

This verifies that the MKV stored directly at the USB root is no longer lost
when descriptor-relative metadata lookup fails on the exFAT mount.

Evidence:
`diagnostics/session-20260723-233538/latest.log`.

### Playback from the library

The single USB-root MKV appeared in the normal library. Pressing Cross produced
a one-item playback request. The source opened as Matroska with 28 streams,
HEVC 1920x1080 video, and AAC 48 kHz stereo audio. Playback heartbeats advanced
without the former player-creation failure.

Evidence:
`diagnostics/session-20260723-233647/previous.log`.

### Direct playback from websrv/HBL

The same file was passed as `argv[1]` through the launcher. Source and player
creation succeeded, and repeated playback heartbeats were emitted. The old
`Requested output sample format -1 is invalid` error did not recur after
changing the player request from unsupported float audio to signed 16-bit
audio.

Evidence:
`diagnostics/session-20260723-233647/latest.log`.

## Still requiring controller verification

These behaviors are implemented and host-tested but are not yet proven by a
PS5 interaction log:

- Touchpad opens the native source browser without an IME or black screen.
- An arbitrary highlighted media file persists and displays as one Movie.
- An arbitrary highlighted folder persists and displays as one TV Show.
- Opening that TV Show displays its naturally ordered episode list.
- Selecting an episode from the grouped view starts playback and returns to
  the same show afterward.

Do not mark the source-selection goal complete until those events appear in a
fresh PS5 log and the rendered behavior is confirmed by the user.
