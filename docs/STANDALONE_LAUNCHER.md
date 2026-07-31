# Standalone dashboard launcher

The release payload is `bfplayer-standalone.elf`. It is the only PS5
file required for installation and normal startup.

The payload:

1. contains the complete stripped BFplayer player ELF in gzip-compressed form;
2. streams that image through a fixed 64 KiB buffer to an atomically replaced,
   CRC-verified `/data/homebrew/BFplayer/bfplayer.elf`, alongside the runtime
   font, icon, and manifest assets;
3. creates or repairs the Media-category `/system_ex/app/PSMC00001` BigApp
   host files;
4. registers the English-only `PSMC00001` dashboard tile;
5. uninstalls the obsolete `PSMR00001` Games registration from alpha.11/12;
6. keeps a minimal HTTP listener bound only to `127.0.0.1:9040`;
7. maps the verified installed player read-only into the required PS5 BigApp
   context when the tile requests `/launch`, avoiding a second full-size
   decompression allocation.

After a successful handoff, the loopback route returns an empty HTTP 204
response instead of deliberately creating a launcher document. If the BigApp
handoff fails, the route returns a short error page so the failure remains
visible and recoverable.

After successful injection setup, the resident launcher sends a native PS5
notification confirming that BFplayer is loaded and available in Media.
When the user chooses Exit BFplayer, the native player closes its
resources and terminates the active BigApp host through SystemService so the
console can return to PlayStation Home. It verifies that the active title is
`PSMC00001` first and refuses to terminate an unrelated BigApp.

Reinjecting a newer standalone payload is an in-place update. The new process
obtains an exclusive instance lock, asks the prior resident launcher to shut
down through its loopback-only endpoint, completes that handover, refreshes
runtime assets and the existing `PSMC00001` registration, and binds the
replacement port 9040 service only when the update is complete. This keeps one
resident service and one Media tile while preserving
`/data/BFplayer/library.db`, settings, sources, playback history, and
logs. Concurrent injections serialize on the same lock rather than running
the update sequence twice. Managed fonts, icons, and manifests are compared
before writing and are atomically replaced only when their bytes differ.

It does not start, link, load, or contact `ps5-payload-websrv`. It does not use
port 8080, the websrv catalog, `homebrew.js`, or the HBL file picker. The HTTP
listener is not reachable from the LAN and implements only `GET /launch`.

The SDK wrapper still records `libkernel_web.sprx` as a normal low-level PS5
runtime stub dependency. That system library is not the `ps5-payload-websrv`
application and does not create a websrv process or listen on port 8080.

The single `PSMC00001` registration and its repaired system host both use
`applicationCategoryType: 65536`, so BFplayer is listed only in Media.

The payload must be injected once after each jailbreak because the minimal
loopback launcher is a resident process. The installed dashboard tile remains,
but selecting it while the payload is not resident cannot start the player.

`/data/homebrew/BFplayer` does not need to exist before injection and
does not need to be retained between jailbreaks. The payload recreates its
verified player image, font, icon, and manifest there. Older builds appeared
to require HBL because they launched through HBL's registered `FAKE00000`
host; removing HBL removed that registration. This build owns its `PSMC00001`
host and no longer shares HBL state.

Logs are written to:

```text
/data/BFplayer/standalone-launcher.log
/data/BFplayer/player-stdio.log
/data/BFplayer/logs/latest.log
/data/BFplayer/logs/previous.log
```

## Runtime boundary

The BigApp transition is based on the GPL-3.0-or-later loader core from
John Tornblom's `ps5-payload-websrv`, but the web server application itself is
not embedded. Only the process-launch, ptrace, and in-memory ELF replacement
modules are retained. BFplayer's launcher supplies its own narrow
loopback listener and passes the read-only mapped, CRC-verified runtime image
to the retained ELF-replacement core.

The build has offline structural verification. A successful PS5 dashboard
launch still requires hardware validation on the owner's firmware and loader
stack.
