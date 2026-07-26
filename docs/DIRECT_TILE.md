# Direct dashboard tile

The `PSMC00001` Media-category tile now targets:

```text
http://127.0.0.1:9040/launch
```

That endpoint belongs to `ps5-media-center-standalone.elf`, not websrv. It is
bound to loopback only, implements one route, and is not reachable from the
LAN. Selecting the tile immediately launches the player ELF embedded inside
the resident standalone payload.

The old `127.0.0.1:8080/hbldr` design, separate tile installer, websrv catalog
package, and `homebrew.js` compatibility launcher were removed in alpha.10.
Alpha.11 also removes the accidental dependency on HBL's `FAKE00000`
registration. Alpha.13 consolidates the runtime host and dashboard entry onto
the single Media-category `PSMC00001` title and removes the obsolete
`PSMR00001` Games registration.

See [STANDALONE_LAUNCHER.md](STANDALONE_LAUNCHER.md) for the complete runtime
and logging design.
