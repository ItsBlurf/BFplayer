# Runtime diagnostics

The standalone launcher and player write:

    /data/BFplayer/standalone-launcher.log
    /data/BFplayer/player-stdio.log
    /data/BFplayer/logs/latest.log
    /data/BFplayer/logs/previous.log

The launcher log records asset installation, tile registration, loopback bind,
launch requests, embedded byte count, and the resulting player PID or failure.
player-stdio.log captures stdout/stderr inherited by the launched BigApp.

latest.log is the current run. At startup, the previous latest.log is rotated
to previous.log, so a crash from the last run remains available. Each file is
capped at 8 MiB and flushed after every record. Fatal signal handlers write a
final crash marker directly to the log descriptor when possible.

The log includes:

- ordered `BFPLAYER_BOOT_STAGE`/`boot-stage` markers spanning process entry,
  singleton locking, legacy migration, diagnostics, SDL, SDL_kitchensink,
  window, renderer, controller, and main-loop entry;
- UTC timestamps, severity, process ID, working directory, system identity,
  arguments, and the installed build-manifest.json;
- SDL and SDL_kitchensink versions plus SDL internal warnings/errors;
- FFmpeg warnings/errors and libass messages;
- local/network source policy, redacted source paths, format and stream
  inventory, decoder/audio/video setup, and playback heartbeats every five
  seconds;
- local/network buffer policy, decoded and compressed queue occupancy, SDL
  audio queue milliseconds, delivered and missed video-frame rates, and
  process peak RSS;
- source and output dimensions, true-4K state, HDR transfer and color metadata,
  tone-map activity, worker count, per-frame tone-map cost, and frame count;
- process user/system CPU time, effective core use, voluntary/involuntary
  context switches, media bytes and calls, read/seek time, and render-loop,
  audio-pull, video-pull, render, and present averages, maxima, and percentiles;
- DLNA discovery, device-description, directory-browse, cancellation,
  truncation, and selected-resource events without signed URL secrets;
- pause, seek, chapter, volume, mute, aspect, subtitle-delay, subtitle-track,
  audio-track, video-track, queue, resume, and shutdown events;
- library roots, scan counts, skipped symlinks/devices, metadata probes,
  database failures, artwork failures, playlist failures, and fatal I/O errors;
- external subtitle decode, cue, bitmap, cache, and render failures.

The logger deliberately does not record credentials. Network URLs are redacted
before being written.

## Hardware automation

While the player is running, alpha.42 starts a bounded authenticated control
endpoint on TCP port 9042. It supports status, open, play, pause, seek, stop,
and exit commands for the repository's PS5 playback harness. A new random
32-byte token is written to `/data/BFplayer/automation.json` with mode 0600 for
each process and the file is removed on clean shutdown. Requests are limited
to 16 KiB, media paths to 4096 bytes, and the command queue to 32 entries.

`tools/hardware-playback-test.mjs` uses this endpoint to run the 4K WebM and
1080p regression sequence without controller input. It records every status
sample in `timeline.jsonl`, writes assertion results to `summary.json`, and
collects the player logs. The endpoint is intended only for testing on a
trusted local network and must not be forwarded to the internet.

## Collecting logs

After reproducing a problem, leave the application stopped and copy all files
from the PS5 before starting another run. The easiest route is the PS5 FTP
server on port 2121. From the project directory, use the collection helper:

    .\collect-logs.ps1 -ConsoleHost '<PS5_IP>'

It creates a timestamped diagnostics/session-* folder containing the available
logs, the matching local build-manifest.json, collection.txt with byte counts
and SHA-256 hashes, and diagnostic-summary.txt with important marker counts,
last matches, and bounded log tails. If the FTP client
cannot address the absolute path, use the PS5 file browser or shell service to
copy the same two files to a reachable staging directory, then download them.
Also send the exact media/subtitle filenames, the button sequence immediately
before the problem, and the matching build manifest.

Do not repeatedly relaunch after a crash before collecting the logs: the next
startup rotates the current log and can obscure the most useful tail.
