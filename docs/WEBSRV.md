# BFplayer for websrv

`BFplayer-websrv.zip` contains the player as a normal websrv homebrew folder.
It does not need `bfplayer-standalone.elf` or BFplayer's resident loopback
launcher.

Extract the ZIP into one of websrv's homebrew roots. The final layout must be
one of:

```text
/data/homebrew/BFplayer/eboot.elf
/mnt/usbN/homebrew/BFplayer/eboot.elf
/mnt/extN/homebrew/BFplayer/eboot.elf
```

The same `BFplayer` folder also contains:

```text
assets/fonts/NotoSans-Regular.ttf
assets/fonts/OFL.txt
sce_sys/icon0.png
build-manifest.json
INSTALL.md
LICENSE
THIRD_PARTY_NOTICES.md
```

Start websrv, open its homebrew list, and select BFplayer. Websrv discovers
`eboot.elf`, supplies the folder as the working directory, creates the BigApp
context, and launches the player directly.

The package deliberately does not contain `homebrew.js`. No custom picker or
extra launcher is needed because websrv already recognizes `eboot.elf`.
BFplayer accepts websrv's `FAKE00000` BigApp host for its clean exit path, while
the standalone release continues to use `PSMC00001`.

The application folder can be placed on internal storage, USB, or extended
storage. Library data, settings, subtitles, resume positions, and logs remain
under `/data/BFplayer`.

Do not launch a second copy while BFplayer is already open. The player lock
will reject the duplicate, but closing the existing player first gives websrv
a clean BigApp transition.

The build verifies the ZIP inventory and extracts every entry for a second
hash comparison. The package can also be checked independently on Windows:

```powershell
.\tools\verify-websrv-package.ps1
```
