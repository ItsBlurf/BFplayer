const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const main = fs.readFileSync(path.join(root, "src/main.cpp"), "utf8");

const orderedStages = [
  "process-entry",
  "player-lock-acquire-begin",
  "player-lock-acquired",
  "legacy-migration-begin",
  "legacy-migration-complete",
  "diagnostics-ready",
  "sdl-init-begin",
  "sdl-init-complete",
  "kitchensink-init-begin",
  "kitchensink-init-complete",
  "window-create-begin",
  "window-create-complete",
  "renderer-create-begin",
  "renderer-create-complete",
  "controller-open-complete",
];

let previous = -1;
for (const stage of orderedStages) {
  const position = main.indexOf(`"${stage}"`);
  assert(position > previous, `startup stage missing or out of order: ${stage}`);
  previous = position;
}

assert(main.includes("BFPLAYER_BOOT_STAGE stage=%s pid=%ld"));
assert(main.includes('boot_stage("library-loop-enter")'));
assert(main.includes('boot_stage("player-loop-enter")'));

const collector = fs.readFileSync(path.join(root, "collect-logs.ps1"), "utf8");
for (const marker of [
  "BFPLAYER_BOOT_STAGE",
  "boot-stage stage=",
  "BFPLAYER_FATAL_SIGNAL",
  "playback-heartbeat ",
]) {
  assert(collector.includes(`'${marker}'`), `collector omits ${marker}`);
}
assert(collector.includes("diagnostic-summary.txt"));

console.log("startup diagnostics tests passed");
