# Research and architecture decisions

## VLC display behavior

The authoritative VLC Desktop documentation separates **Aspect Ratio** from
**Crop**:

- Aspect Ratio: Default, 1:1, 4:3, 16:9, 16:10, 2.21:1, 2.35:1, 2.39:1, 5:4.
- Crop: Default, 16:10, 16:9, 1.85:1, 2.21:1, 2.35:1, 2.39:1, 5:3, 4:3,
  5:4, 1:1.

Source:
[VLC Desktop Video documentation](https://docs.videolan.me/vlc-user/desktop/3.0/en/basic/video.html).

VLC for Android additionally describes Best fit, Fit screen, Fill, 16:9, 4:3,
and Center as screen-ratio presets. BFplayer expresses the useful
fullscreen behaviors without conflating them with exact aspect overrides:
Best fit, Fill screen (crop), and Fullscreen stretch. Exact 16:9 and 4:3 remain
in the aspect list.

The geometry implementation uses this order:

```text
decoded frame + sample aspect
    -> exact source crop
    -> optional display-aspect override
    -> best-fit / fill-screen / fullscreen-stretch output
```

This order preserves anamorphic video correctly and makes a crop remove source
pixels without stretching the remaining picture.

## PS5 launch model

The upstream
[`ps5-payload-dev/websrv`](https://github.com/ps5-payload-dev/websrv)
implementation exposes `/hbldr`. Its request handler accepts `path`, `cwd`,
`args`, `env`, `pipe`, and `daemon`; the normal path calls
`sys_launch_homebrew`, which creates the BigApp context before replacing it
with the requested Prospero ELF.

The current player depends on that BigApp display/audio context. A plain
elfldr daemon process is not equivalent. A PS4/OpenOrbis Fake-PKG is also not
equivalent: it uses the PS4 compatibility process and CRT/ABI, while the
player is an `x86_64-sie-ps5` Prospero DYN payload linked against PacBrew's PS5
SDL/FFmpeg stack.

Alpha.10 retains the proven BigApp transition without retaining the websrv
application:

1. One resident ELF embeds the complete player and required small assets.
2. The same ELF registers the Media-category tile with `AppInstallTitleDir`.
3. A raw-socket listener binds only `127.0.0.1:9040` and accepts only
   `GET /launch`.
4. The launch route passes the embedded player bytes directly to the retained
   `hbldr`/ptrace/ELF-replacement core.
5. No libmicrohttpd, websrv process, port 8080, catalog, `homebrew.js`, or HBL
   picker participates in normal startup.

This remains a resident payload plus dashboard tile, not a signed standalone
PS5 title. The public BigApp loader core is GPL-3.0-or-later and is retained
with source, modification notes, and the complete license.

## Visual direction

The original screenshot had a large unstructured empty field, oversized status
and control text, and a generic glossy icon. The redesign uses:

- a fixed 1920x1080 grid with compact header and footer rails;
- dark navy panel hierarchy with blue outlines and restrained gold accents;
- a selected-row bar instead of a full bright rectangle;
- a real empty-library instruction and artwork placeholder;
- a pixel-art blade/play mark aligned with the BFpilot visual language;
- an icon designed for strong silhouette recognition at dashboard size.

`tools/render-ui-preview.ps1` produces the checked-in deterministic empty-state
preview. It mirrors the SDL layout constants but is not a substitute for PS5
runtime verification.
