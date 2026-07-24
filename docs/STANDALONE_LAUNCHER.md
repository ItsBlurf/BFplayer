# Standalone dashboard launcher

The release payload is `ps5-media-center-standalone.elf`. It is the only PS5
file required for installation and normal startup.

The payload:

1. contains the complete Media Center player ELF;
2. writes its small runtime font, icon, and manifest assets under
   `/data/homebrew/PS5-MediaCenter`;
3. registers the English-only `PSMC00001` dashboard tile;
4. keeps a minimal HTTP listener bound only to `127.0.0.1:9040`;
5. launches the embedded player into the required PS5 BigApp context when the
   tile requests `/launch`.

It does not start, link, load, or contact `ps5-payload-websrv`. It does not use
port 8080, the websrv catalog, `homebrew.js`, or the HBL file picker. The HTTP
listener is not reachable from the LAN and implements only `GET /launch`.

The SDK wrapper still records `libkernel_web.sprx` as a normal low-level PS5
runtime stub dependency. That system library is not the `ps5-payload-websrv`
application and does not create a websrv process or listen on port 8080.

The payload must be injected once after each jailbreak because the minimal
loopback launcher is a resident process. The installed dashboard tile remains,
but selecting it while the payload is not resident cannot start the player.

Logs are written to:

```text
/data/PS5-MediaCenter/standalone-launcher.log
/data/PS5-MediaCenter/player-stdio.log
/data/PS5-MediaCenter/logs/latest.log
/data/PS5-MediaCenter/logs/previous.log
```

## Runtime boundary

The BigApp transition is based on the GPL-3.0-or-later loader core from
John Törnblom's `ps5-payload-websrv`, but the web server application itself is
not embedded. Only the process-launch, ptrace, and in-memory ELF replacement
modules are retained. PS5 Media Center's launcher supplies its own narrow
loopback listener and launches the player directly from the bytes embedded in
the standalone payload.

The build has offline structural verification. A successful PS5 dashboard
launch still requires hardware validation on the owner's firmware and loader
stack.
