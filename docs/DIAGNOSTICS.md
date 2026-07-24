# Runtime diagnostics

The standalone launcher and player write:

    /data/PS5-MediaCenter/standalone-launcher.log
    /data/PS5-MediaCenter/player-stdio.log
    /data/PS5-MediaCenter/logs/latest.log
    /data/PS5-MediaCenter/logs/previous.log

The launcher log records asset installation, tile registration, loopback bind,
launch requests, embedded byte count, and the resulting player PID or failure.
player-stdio.log captures stdout/stderr inherited by the launched BigApp.

latest.log is the current run. At startup, the previous latest.log is rotated
to previous.log, so a crash from the last run remains available. Each file is
capped at 8 MiB and flushed after every record. Fatal signal handlers write a
final crash marker directly to the log descriptor when possible.

The log includes:

- UTC timestamps, severity, process ID, working directory, system identity,
  arguments, and the installed build-manifest.json;
- SDL and SDL_kitchensink versions plus SDL internal warnings/errors;
- FFmpeg warnings/errors and libass messages;
- local/network source policy, redacted source paths, format and stream
  inventory, decoder/audio/video setup, and playback heartbeats every five
  seconds;
- pause, seek, chapter, volume, mute, aspect, subtitle-delay, subtitle-track,
  audio-track, video-track, queue, resume, and shutdown events;
- library roots, scan counts, skipped symlinks/devices, metadata probes,
  database failures, artwork failures, playlist failures, and fatal I/O errors;
- external subtitle decode, cue, bitmap, cache, and render failures.

The logger deliberately does not record credentials. Network URLs are redacted
before being written.

## Collecting logs

After reproducing a problem, leave the application stopped and copy all files
from the PS5 before starting another run. The easiest route is the PS5 FTP
server on port 2121. From the project directory, use the collection helper:

    .\collect-logs.ps1 -ConsoleHost '<PS5_IP>'

It creates a timestamped diagnostics/session-* folder containing the available logs,
the matching local build-manifest.json, and collection.txt with byte counts and
SHA-256 hashes. If the FTP client
cannot address the absolute path, use the PS5 file browser or shell service to
copy the same two files to a reachable staging directory, then download them.
Also send the exact media/subtitle filenames, the button sequence immediately
before the problem, and the matching build manifest.

Do not repeatedly relaunch after a crash before collecting the logs: the next
startup rotates the current log and can obscure the most useful tail.
