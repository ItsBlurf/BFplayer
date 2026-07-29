#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";

function parseArguments(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    const name = argv[index];
    const value = argv[index + 1];
    if (!name?.startsWith("--") || value === undefined) {
      throw new Error(`Invalid argument near ${name ?? "<end>"}`);
    }
    values[name.slice(2)] = value;
  }
  if (!values.host || !values.webm || !values.romance || !values.output) {
    throw new Error(
      "Required: --host HOST --webm PATH --romance PATH --output DIRECTORY",
    );
  }
  return {
    host: values.host,
    bfpilotPort: Number(values["bfpilot-port"] ?? 5905),
    webm: values.webm,
    romance: values.romance,
    output: path.resolve(values.output),
  };
}

const options = parseArguments(process.argv.slice(2));
fs.mkdirSync(options.output, { recursive: true });
const timelinePath = path.join(options.output, "timeline.jsonl");
const summaryPath = path.join(options.output, "summary.json");

function record(type, details = {}) {
  const item = {
    capturedUtc: new Date().toISOString(),
    type,
    ...details,
  };
  fs.appendFileSync(timelinePath, `${JSON.stringify(item)}\n`, "utf8");
  return item;
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function request(url, init = {}, timeoutMs = 4000) {
  const response = await fetch(url, {
    ...init,
    signal: AbortSignal.timeout(timeoutMs),
  });
  const body = await response.text();
  let parsed;
  try {
    parsed = JSON.parse(body);
  } catch {
    throw new Error(`Non-JSON response ${response.status}: ${body.slice(0, 200)}`);
  }
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}: ${JSON.stringify(parsed)}`);
  }
  return parsed;
}

async function readAutomation() {
  const response = await readBfpilotText("/data/BFplayer/automation.json");
  return JSON.parse(response);
}

async function readBfpilotText(fileName) {
  const filePath = encodeURIComponent(fileName);
  const endpoint =
    `http://${options.host}:${options.bfpilotPort}/api/fs/text?path=${filePath}`;
  const response = await request(endpoint);
  if (!response.ok || typeof response.text !== "string") {
    throw new Error(`BFPilot did not return ${fileName}`);
  }
  return response.text;
}

const automation = await readAutomation();
const remoteRoot = `http://${options.host}:${automation.port}`;
const headers = { "X-BFplayer-Token": automation.token };
record("test-start", {
  build: automation.build,
  pid: automation.pid,
  remotePort: automation.port,
});

async function remote(route, method = "GET", query = {}) {
  const url = new URL(`${remoteRoot}${route}`);
  for (const [name, value] of Object.entries(query)) {
    url.searchParams.set(name, String(value));
  }
  const response = await request(url, { method, headers });
  record("command", { route, method, query, response });
  return response;
}

async function status(label) {
  try {
    const state = await request(`${remoteRoot}/v1/status`, { headers });
    record("status", { label, state });
    return state;
  } catch (error) {
    record("status-error", { label, error: String(error) });
    throw error;
  }
}

async function waitFor(label, predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const state = await status(label);
    if (predicate(state)) {
      return state;
    }
    await delay(250);
  }
  throw new Error(`Timed out waiting for ${label}`);
}

async function sampleFor(label, durationMs, stopWhen = () => false) {
  const samples = [];
  const deadline = Date.now() + durationMs;
  while (Date.now() < deadline) {
    const state = await status(label);
    samples.push(state);
    if (stopWhen(state, samples)) {
      break;
    }
    await delay(500);
  }
  return samples;
}

function playbackSamples(samples, mediaPath) {
  return samples.filter(
    (sample) => sample.phase === "playback" && sample.mediaPath === mediaPath,
  );
}

function maximum(samples, field) {
  const values = samples
    .map((sample) => Number(sample[field]))
    .filter(Number.isFinite);
  return values.length ? Math.max(...values) : null;
}

function median(samples, field) {
  const values = samples
    .map((sample) => Number(sample[field]))
    .filter(Number.isFinite)
    .sort((left, right) => left - right);
  return values.length ? values[Math.floor(values.length / 2)] : null;
}

function medianPositive(samples, field) {
  const values = samples
    .map((sample) => Number(sample[field]))
    .filter((value) => Number.isFinite(value) && value > 0)
    .sort((left, right) => left - right);
  return values.length ? values[Math.floor(values.length / 2)] : null;
}

const result = {
  build: automation.build,
  webm: {},
  pause: {},
  seek: {},
  audioSwitch: {},
  subtitleSwitch: {},
  romance: {},
  assertions: [],
};

try {
  const initial = await status("initial");
  if (initial.phase === "playback") {
    await remote("/v1/stop", "POST");
    await waitFor("initial-stop", (state) => state.phase === "library", 10000);
  }

  await remote("/v1/open", "POST", { path: options.webm });
  const webmStart = await waitFor(
    "webm-start",
    (state) => state.phase === "playback" && state.mediaPath === options.webm,
    15000,
  );
  const webmSamples = await sampleFor(
    "webm-full",
    45000,
    (state, samples) =>
      samples.length > 4 &&
      state.phase === "library",
  );
  const webmPlayback = playbackSamples(webmSamples, options.webm);
  result.webm = {
    sourceWidth: webmStart.sourceWidth,
    sourceHeight: webmStart.sourceHeight,
    outputWidth: webmStart.outputWidth,
    outputHeight: webmStart.outputHeight,
    sourceFps: webmStart.sourceFps,
    hdrSource: webmStart.hdrSource,
    hdrTransfer: webmStart.hdrTransfer,
    hdrToneMapActive: webmPlayback.some(
      (sample) => sample.hdrToneMapActive === true,
    ),
    hdrToneMapFrames: maximum(webmPlayback, "hdrToneMapFrames"),
    hdrToneMapAverageMs: maximum(webmPlayback, "hdrToneMapAverageMs"),
    hdrToneMapWorkers: maximum(webmPlayback, "hdrToneMapWorkers"),
    hdrSourcePeakNits: maximum(webmPlayback, "hdrSourcePeakNits"),
    hdrTargetPeakNits: maximum(webmPlayback, "hdrTargetPeakNits"),
    maxPositionSeconds: maximum(webmPlayback, "positionSeconds"),
    medianDeliveredFps: medianPositive(webmPlayback, "deliveredFps"),
    maxDeliveredFps: maximum(webmPlayback, "deliveredFps"),
    maxPresentP95Ms: maximum(webmPlayback, "presentP95Ms"),
    maxVideoPullMs: maximum(webmPlayback, "videoPullAverageMs"),
    maxCpuCores: maximum(webmPlayback, "cpuCoreEquivalents"),
    maxPeakRssKiB: maximum(webmPlayback, "peakRssKiB"),
    returnedToLibrary: webmSamples.at(-1)?.phase === "library",
  };

  await remote("/v1/open", "POST", { path: options.webm });
  await waitFor(
    "pause-run-start",
    (state) =>
      state.phase === "playback" &&
      state.mediaPath === options.webm &&
      state.positionSeconds >= 4,
    20000,
  );
  await remote("/v1/pause", "POST");
  const paused = await waitFor("pause-applied", (state) => state.paused, 5000);
  const pausedSamples = await sampleFor("paused", 6500);
  result.pause = {
    startPositionSeconds: paused.positionSeconds,
    endPositionSeconds: pausedSamples.at(-1)?.positionSeconds ?? null,
    positionDriftSeconds:
      (pausedSamples.at(-1)?.positionSeconds ?? paused.positionSeconds) -
      paused.positionSeconds,
    maximumDeliveredFps: maximum(pausedSamples, "deliveredFps"),
    finalDeliveredFps: pausedSamples.at(-1)?.deliveredFps ?? null,
  };

  await remote("/v1/play", "POST");
  await waitFor("resume-applied", (state) => !state.paused, 5000);
  const beforeSeek = await status("before-seek");
  const seekTargetSeconds = Math.min(
    beforeSeek.durationSeconds,
    beforeSeek.positionSeconds + 10,
  );
  const seekIssuedAt = Date.now();
  await remote("/v1/seek", "POST", { seconds: 10 });
  const afterSeek = await waitFor(
    "seek-recovered",
    (state) =>
      state.phase === "playback" &&
      state.mediaSeekCalls > beforeSeek.mediaSeekCalls &&
      state.positionSeconds >= seekTargetSeconds - 0.5,
    15000,
  );
  result.seek = {
    fromSeconds: beforeSeek.positionSeconds,
    targetSeconds: seekTargetSeconds,
    recoveredSeconds: afterSeek.positionSeconds,
    positionErrorSeconds: Math.abs(
      afterSeek.positionSeconds - seekTargetSeconds,
    ),
    recoveryMilliseconds: Date.now() - seekIssuedAt,
    deliveredFps: afterSeek.deliveredFps,
  };
  await remote("/v1/stop", "POST");
  await waitFor("webm-stop", (state) => state.phase === "library", 10000);

  await remote("/v1/open", "POST", { path: options.romance });
  const romanceStart = await waitFor(
    "romance-start",
    (state) =>
      state.phase === "playback" && state.mediaPath === options.romance,
    15000,
  );
  const romanceSamples = await sampleFor("romance-steady", 20000);

  const audioBefore = await status("audio-switch-before");
  const audioSwitchAt = Date.now();
  await remote("/v1/cycle-audio", "POST");
  const audioAfter = await waitFor(
    "audio-switch-recovered",
    (state) =>
      state.phase === "playback" &&
      state.audioStream !== audioBefore.audioStream &&
      state.mediaSeekCalls > audioBefore.mediaSeekCalls,
    5000,
  );
  const audioSwitchMilliseconds = Date.now() - audioSwitchAt;
  result.audioSwitch = {
    previousStream: audioBefore.audioStream,
    currentStream: audioAfter.audioStream,
    recoveryMilliseconds: audioSwitchMilliseconds,
    positionDiscontinuitySeconds: Math.abs(
      (audioAfter.positionSeconds - audioBefore.positionSeconds) -
        audioSwitchMilliseconds / 1000,
    ),
  };

  let subtitleBefore = await status("subtitle-switch-before");
  let subtitleSwitchAt = Date.now();
  await remote("/v1/cycle-subtitle", "POST");
  let subtitleAfter = await waitFor(
    "subtitle-switch-selected",
    (state) =>
      state.phase === "playback" &&
      state.subtitleStream !== subtitleBefore.subtitleStream,
    5000,
  );
  if (subtitleAfter.subtitleStream < 0) {
    subtitleBefore = subtitleAfter;
    subtitleSwitchAt = Date.now();
    await remote("/v1/cycle-subtitle", "POST");
    subtitleAfter = await waitFor(
      "subtitle-switch-recovered",
      (state) =>
        state.phase === "playback" &&
        state.subtitleStream >= 0 &&
        state.mediaSeekCalls > subtitleBefore.mediaSeekCalls,
      5000,
    );
  }
  const subtitleSwitchMilliseconds = Date.now() - subtitleSwitchAt;
  result.subtitleSwitch = {
    previousStream: subtitleBefore.subtitleStream,
    currentStream: subtitleAfter.subtitleStream,
    recoveryMilliseconds: subtitleSwitchMilliseconds,
    positionDiscontinuitySeconds: Math.abs(
      (subtitleAfter.positionSeconds - subtitleBefore.positionSeconds) -
        subtitleSwitchMilliseconds / 1000,
    ),
  };

  const romanceBeforeSeek = await status("romance-before-seek");
  const romanceSeekTargetSeconds = Math.min(
    romanceBeforeSeek.durationSeconds,
    romanceBeforeSeek.positionSeconds + 10,
  );
  const romanceSeekAt = Date.now();
  await remote("/v1/seek", "POST", { seconds: 10 });
  const romanceAfterSeek = await waitFor(
    "romance-seek-recovered",
    (state) =>
      state.mediaSeekCalls > romanceBeforeSeek.mediaSeekCalls &&
      state.positionSeconds >= romanceSeekTargetSeconds - 0.5,
    15000,
  );
  const romanceSeekRecoveryMilliseconds = Date.now() - romanceSeekAt;
  const romanceRecoverySamples = await sampleFor("romance-recovery", 8000);
  const romancePlayback = playbackSamples(
    [...romanceSamples, ...romanceRecoverySamples],
    options.romance,
  );
  result.romance = {
    sourceWidth: romanceStart.sourceWidth,
    sourceHeight: romanceStart.sourceHeight,
    outputWidth: romanceStart.outputWidth,
    outputHeight: romanceStart.outputHeight,
    sourceFps: romanceStart.sourceFps,
    medianDeliveredFps: medianPositive(romancePlayback, "deliveredFps"),
    maxPresentP95Ms: maximum(romancePlayback, "presentP95Ms"),
    maxCpuCores: maximum(romancePlayback, "cpuCoreEquivalents"),
    seekRecoveryMilliseconds: romanceSeekRecoveryMilliseconds,
    seekTargetSeconds: romanceSeekTargetSeconds,
    recoveredPositionSeconds: romanceAfterSeek.positionSeconds,
    seekPositionErrorSeconds: Math.abs(
      romanceAfterSeek.positionSeconds - romanceSeekTargetSeconds,
    ),
  };
  await remote("/v1/stop", "POST");
  await waitFor("romance-stop", (state) => state.phase === "library", 10000);

  const pauseDrift = Math.abs(result.pause.positionDriftSeconds);
  result.assertions = [
    {
      name: "adaptive-4k60-software-output",
      passed:
        result.webm.sourceWidth === 3840 &&
        result.webm.sourceHeight === 2160 &&
        result.webm.outputWidth === 1920 &&
        result.webm.outputHeight === 1080,
    },
    {
      name: "webm-completes-once",
      passed:
        result.webm.returnedToLibrary &&
        result.webm.maxPositionSeconds >= 27,
    },
    {
      name: "hdr-tone-map-active",
      passed:
        result.webm.hdrSource === true &&
        result.webm.hdrToneMapActive === true &&
        result.webm.hdrToneMapFrames > 0,
    },
    {
      name: "hdr-tone-map-frame-cost",
      passed:
        Number.isFinite(result.webm.hdrToneMapAverageMs) &&
        result.webm.hdrToneMapAverageMs <= 5,
    },
    {
      name: "webm-realtime-playback",
      passed:
        Number.isFinite(result.webm.medianDeliveredFps) &&
        result.webm.medianDeliveredFps >=
          result.webm.sourceFps * 0.95,
    },
    {
      name: "webm-frame-pipeline-budget",
      passed:
        Number.isFinite(result.webm.maxVideoPullMs) &&
        result.webm.maxVideoPullMs <= 8 &&
        Number.isFinite(result.webm.maxPresentP95Ms) &&
        result.webm.maxPresentP95Ms <= 16,
    },
    {
      name: "bounded-playback-memory",
      passed:
        Number.isFinite(result.webm.maxPeakRssKiB) &&
        result.webm.maxPeakRssKiB <= 900000,
    },
    {
      name: "pause-clock-stable",
      passed: Number.isFinite(pauseDrift) && pauseDrift <= 0.15,
    },
    {
      name: "webm-seek-recovers",
      passed:
        result.seek.recoveryMilliseconds <= 1500 &&
        result.seek.positionErrorSeconds <= 1,
    },
    {
      name: "romance-seek-recovers",
      passed:
        result.romance.seekRecoveryMilliseconds <= 1500 &&
        result.romance.seekPositionErrorSeconds <= 1,
    },
    {
      name: "audio-track-switch-recovers",
      passed:
        result.audioSwitch.currentStream !==
          result.audioSwitch.previousStream &&
        result.audioSwitch.recoveryMilliseconds <= 1500 &&
        result.audioSwitch.positionDiscontinuitySeconds <= 1,
    },
    {
      name: "subtitle-track-switch-recovers",
      passed:
        result.subtitleSwitch.currentStream >= 0 &&
        result.subtitleSwitch.currentStream !==
          result.subtitleSwitch.previousStream &&
        result.subtitleSwitch.recoveryMilliseconds <= 1500 &&
        result.subtitleSwitch.positionDiscontinuitySeconds <= 1,
    },
    {
      name: "romance-realtime-playback",
      passed:
        Number.isFinite(result.romance.medianDeliveredFps) &&
        result.romance.medianDeliveredFps >=
          result.romance.sourceFps * 0.95,
    },
  ];
} catch (error) {
  result.error = error instanceof Error ? error.stack : String(error);
  record("test-error", { error: result.error });
} finally {
  try {
    await remote("/v1/exit", "POST");
  } catch (error) {
    record("exit-error", { error: String(error) });
  }
  const diagnosticFiles = {
    "latest.log": "/data/BFplayer/logs/latest.log",
    "previous.log": "/data/BFplayer/logs/previous.log",
    "standalone-launcher.log": "/data/BFplayer/standalone-launcher.log",
    "player-stdio.log": "/data/BFplayer/player-stdio.log",
  };
  for (const [localName, remoteName] of Object.entries(diagnosticFiles)) {
    try {
      const contents = await readBfpilotText(remoteName);
      fs.writeFileSync(path.join(options.output, localName), contents, "utf8");
    } catch (error) {
      record("log-fetch-error", {
        remoteName,
        error: String(error),
      });
    }
  }
  fs.writeFileSync(summaryPath, `${JSON.stringify(result, null, 2)}\n`, "utf8");
  record("test-end", {
    error: result.error ?? null,
    assertions: result.assertions,
  });
}

if (result.error || result.assertions.some((item) => !item.passed)) {
  process.exitCode = 1;
}
