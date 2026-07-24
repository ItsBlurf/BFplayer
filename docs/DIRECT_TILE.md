# Direct dashboard tile

PS5 Media Center includes a narrow, separate installer payload named
`ps5mc-tile-installer.elf`. It registers the `PSMC00001` Media-category tile
and points it at:

```text
http://127.0.0.1:8080/hbldr
  ?path=/data/homebrew/PS5-MediaCenter/ps5-media-center.elf
  &cwd=/data/homebrew/PS5-MediaCenter
  &pipe=0
```

This is a direct launch. Selecting the PS5 Media Center tile does not open the
websrv catalog, execute `homebrew.js`, or show a file picker. The local websrv
service receives the request and immediately creates the required PS5 BigApp
context for the Media Center ELF.

## Why a local launch service is still required

`ps5-media-center.elf` is a Prospero payload (`ET_DYN`), not a signed PS5
application executable. SDL video/audio homebrew needs a BigApp process. The
working public PS5 payload stack creates that process through `hbldr`; merely
renaming the ELF to `eboot.bin`, embedding it in an OpenOrbis package, or using
an ordinary daemon process does not provide the same ABI or display context.

The direct tile therefore keeps the proven BigApp transition but removes the
visible launcher interface. It requires websrv to already be listening on
localhost port 8080. Configure websrv in the same payload-manager/autoloader
sequence used after each jailbreak. If port 8080 is not running, the tile
cannot start the player and the browser will report a local connection error.

## Install

1. Copy the complete `PS5-MediaCenter` directory to
   `/data/homebrew/PS5-MediaCenter`.
2. Start websrv on port 8080.
3. Inject `ps5mc-tile-installer.elf` once through the existing ELF loader.
4. Return to the dashboard and select **PS5 Media Center**.

The installer only creates or refreshes:

```text
/user/app/PSMC00001/sce_sys/param.json
/user/app/PSMC00001/sce_sys/icon0.png
```

Its log is written to:

```text
/data/PS5-MediaCenter/tile-installer.log
```

The installer carries AppInst privileges because tile registration needs them.
The media player ELF itself does not link AppInst or kernel-system libraries.

## Safety boundary

This project intentionally does not ship the quarantined OpenOrbis/Fake-PKG
experiments. Those packages use a PS4-compatibility process model and did not
provide a safe, proven host for this Prospero SDL payload.
