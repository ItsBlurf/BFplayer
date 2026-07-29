# BFplayer

**AI use notice:** I used ai and specifaclly Codex with gpt 5.5 and 5.6 while developing BFplayer.  I direct the development and have spent many hours across 
multiple days testing builds and playback behavior on my own PS5.

BFplayer is a native media library and player for jailbroken
PlayStation 5 consoles. The standalone release installs its own tile in the
Media section and does not require websrv or Homebrew Launcher. A separate
websrv-ready ZIP is available for people who already use websrv and do not
want BFplayer's standalone launcher running in the background.

<img width="1920" height="1080" alt="20260728_013154_00032112 jpg" src="https://github.com/user-attachments/assets/4f542583-3c66-493d-ac3c-aa42e55ff79e" />
## Features

- Movies, TV shows, seasons, music, and playlists
- Manual media sources with an optional whole-library import
- Resume playback, favorites, search, sorting, and playback queues
- Embedded and external subtitles with timing adjustment
- In-player subtitle browser and optional SubDL search/download
- Audio, video, subtitle, and chapter selection
- Aspect-ratio, crop, and scaling controls
- Local artwork or an automatically generated video preview
- Persistent settings and rotating diagnostic logs
- Clean reinjection: a newer payload replaces the resident launcher without
  creating a second tile or resetting the library

Playback is powered by FFmpeg 7.0.1, SDL2, SDL_kitchensink, and libass. Decoding
is currently software-based, so demanding 4K HEVC or AV1 files may not play
smoothly.

The same limitation applies to 4K 50/60 fps VP9 WebM, especially 10-bit HDR
files. The player uses the 16 logical processors reported by SDL, enlarged
video buffers, and proportional 1080p output conversion for oversized sources,
but these formats can still exceed the PS5 homebrew software-decoding budget.

Every video starts in **Best fit**, **Original** aspect, and **No crop** mode.
The player uses the source stream's display ratio, including anamorphic
pixel-ratio metadata, and falls back to the decoded frame dimensions when that
metadata is unavailable. Manual scale, aspect, and crop overrides apply only
to the current video.

## Install

### Standalone Media tile

Download `bfplayer-standalone.elf` from the latest release and inject
that payload after each jailbreak. It installs or updates the **BFplayer**
tile in the Media section and stays resident to launch the player.

Nothing needs to be copied to `/data/homebrew` beforehand. The payload creates
and maintains its own runtime folder. Reinjection preserves the library,
settings, sources, playback history, and logs. Repeated tile launches are
coalesced, and a player-instance lock prevents two BFplayer processes from
initializing PS5 video memory at the same time.

### websrv

Download `BFplayer-websrv.zip` from the latest release and extract its
`BFplayer` folder into a websrv homebrew root such as `/data/homebrew`,
`/mnt/usb0/homebrew`, or `/mnt/ext0/homebrew`. The final entrypoint is
`BFplayer/eboot.elf`.

websrv launches the player directly, so this method does not use
`bfplayer-standalone.elf`, the Media tile, port 9040, or BFplayer's resident
launcher. See [docs/WEBSRV.md](docs/WEBSRV.md) for the exact package layout.

## Library controls

| Button | Action |
| --- | --- |
| Cross | Open or play |
| Circle | Go back inside a show or season; no action at the library root |
| Triangle | Remove the selected movie or TV source (press twice) |
| D-pad Left/Right | Change library category |
| D-pad Up/Down | Move through the list |
| L1/R1 | Previous or next page |
| L3 | Add or remove favorite |
| R3 | Change sort mode |
| Touchpad | Add media |
| Options | Open the main menu |

The Add Media browser starts at `/`. Cross opens a folder or adds a movie,
Triangle adds a folder as one TV show, and Square imports a folder as a mixed
library. Mixed import treats loose video files as movies and child folders as
TV shows.

## Playback controls

| Button | Action |
| --- | --- |
| Cross | Pause or resume |
| Circle | Change subtitle track |
| Square | Change audio track |
| Triangle | Change video track |
| D-pad Left/Right | Short seek |
| D-pad Down/Up | Long seek |
| L1/R1 | Previous or next chapter |
| L3/R3 | Volume down or up |
| Touchpad | Show the full control list |
| Options | Open playback menu |

Display controls are available through the playback menu and controller
shortcuts, including scaling, aspect ratio, and crop mode.

### External and online subtitles

Open **Options > Subtitles** while a video is playing. The list contains
embedded tracks, matching files found beside the video, and any subtitle chosen
through **Browse subtitle files**. The browser starts in the video's folder and
shows only folders and supported subtitle files.

Optional online search uses the official SubDL API. In **Playback settings**,
select **SubDL API key** and enter a key from a free SubDL account, then set
**Download languages** to comma-separated codes such as `en,ar,fr`. BFplayer
searches by the playing release filename, downloads in the background, stores
the selected file under `/data/BFplayer/subtitles`, and activates it without
restarting playback. The key is sent only in HTTPS request headers and is not
written to logs or URLs. SubDL's own quotas and terms apply.

For fan edits or unusual release names that cannot be identified from the
filename, choose **Online subtitle providers > SubDL > Search by movie or show
title** and enter the name.
The provider screen also lists OpenSubtitles, Podnapisi, and Addic7ed in grey
when they cannot be integrated without an approved API or unsupported website
scraping.

All vertical lists wrap from top to bottom and bottom to top. Holding D-pad Up
or Down accelerates progressively for large libraries and folders.

## Logs

Runtime logs are stored under:

```text
/data/BFplayer/logs
```

The repository includes `collect-logs.ps1` for copying the current and previous
logs from a console. More details are in [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

## Building

The project uses
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and PacBrew v0.37.
From the project directory:

```powershell
. ..\..\ps5dev-env.ps1
.\build.ps1 -Configuration Release
```

Release artifacts are written to `dist/`.

## Notes

- The dashboard title ID is `PSMC00001`.
- The launcher listens only on `127.0.0.1:9040`.
- Network services are intended for a trusted local network.
- Hardware and format behavior can vary by firmware, HEN, storage device, and
  media encoding.

See [docs/STANDALONE_LAUNCHER.md](docs/STANDALONE_LAUNCHER.md) for launcher
details, [docs/WEBSRV.md](docs/WEBSRV.md) for the websrv package, and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled components and
licenses.

## Contributors

- **ItsBlurf:** I started and maintain BFplayer, make the project decisions,
  test it on my PS5, and handle releases.
- **OpenAI Codex:** I use it to help with implementation, review, and
  documentation.

## Acknowledgements

- John Törnblom and
  [ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv) for the
  BigApp and ELF loading foundation and the websrv homebrew format
- [FFmpeg and ffplay](https://ffmpeg.org/) for the media and playback
  foundation
- [PacBrew](https://github.com/ps5-payload-dev/pacbrew), SDL2, and
  SDL_kitchensink for making the PS5 playback stack possible
- [VLC](https://www.videolan.org/vlc/) for the display aspect, crop, and
  scaling behavior used as a reference

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
