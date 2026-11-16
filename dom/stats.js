// stats.js — the stats panel (top right) and the Stats button (bottom left)
// that shows and hides it.
//
// Once a second it samples the counters the app and the renderer keep in the
// bridge block's stats section (see bridge_stats in app/src/bridge.h): no
// messages, nothing measured here. What it shows:
//
//   display       the display's refresh rate, as the browser's frame clock
//                 runs it: measured here, from this thread's own
//                 requestAnimationFrame timestamps (see measureRefresh). On
//                 a Mac with screens of different rates, this is the rate
//                 Chrome paces the window at, which is what matters here.
//   render        frames presented per second. This is capped by the
//                 display (60 on a 60 Hz screen, 120 on a 120 Hz one).
//   missed        display refreshes that went by without a new frame, in
//                 the last sample and in total since the app started: the
//                 renderer's frames were too slow (or arrived unevenly) for
//                 the display. Any steady count here is visible stutter;
//                 lower the resolution or the anti-aliasing in Settings.
//                 Counted exactly: the renderer sums the time between its
//                 frames' timestamps (frame_span_us), and that time divided
//                 by the refresh period is how many refreshes went by.
//   gpu time      the GPU's time per frame, from the renderer's timestamp
//                 queries: the number that says whether a frame can make the
//                 display's refresh (8.3 ms at 120 Hz). `n/a` on a GPU
//                 without timestamp queries.
//   cpu time      the render worker's CPU time per frame: what a frame
//                 costs, whatever the display's refresh rate.
//   physics time  the app's CPU time per fixed step (stepping Box3D and the
//                 rest of app_step). At 240 steps a second the budget is
//                 about 4.2 ms; watch it grow as balls pile up.
//   physics       fixed steps per second (240 when all is well).
//   sync          whether the picture keeps in step with the simulation:
//                 normally `ok · lag N`, where N is how many steps the
//                 renderer's display cursor trails the newest one (it grows
//                 to absorb uneven step publishing; see
//                 renderer/src/cursor.rs). It turns into a red
//                 `ROUGH ±E · lag N` only when that is genuinely failing (the
//                 SYNC_ROUGH_* constants below); a one-off spike (a tab
//                 switch, an anti-aliasing recompile) never trips it.
//   box3d         how many threads Box3D steps the world on, counting the
//                 app's own (web_main.c decides, from the number of cores).
//   balls dropped the balls on the table besides the red one.
//   last cue      the latest impact cue from the app, and its intensity.
//
// Rates are counter differences divided by the REAL time between samples
// (performance.now()), then smoothed. A one-second timer is not really one
// second apart, and in a background tab it is throttled, so assuming 1000 ms
// makes the numbers flicker, or go absurd around a tab switch.

import { STATS_STORAGE_KEY, coarsePointer, storeBool, storedBool } from './settings.js';

const SAMPLE_INTERVAL_MS = 1000;
// Smoothing weight of each new sample: a real change (a lower resolution, a
// pile of balls) shows within 2–3 samples, while the readout holds steady
// against noise.
const EMA_ALPHA = 0.35;
// A sample window is only trusted between these lengths (ms). Shorter: two
// timer callbacks ran almost together, and the quotient is noise. Longer: the
// tab was hidden or throttled, so frames were not running and the window
// measures the stall. Either way the next window starts afresh.
const MIN_WINDOW_MS = 200;
const MAX_WINDOW_MS = 3000;
// The sync line turns red when, for SYNC_ROUGH_WINDOWS windows in a row, the
// display cursor's worst error reached SYNC_ROUGH_ERR steps while its lag was
// at (or within a step of) its ceiling of 24: the unevenness is more than the
// lag may absorb, and motion visibly pulses. Below the ceiling the lag still
// has room to grow, and a single rough window is an event, not a condition.
const SYNC_ROUGH_ERR = 3.0;
const SYNC_ROUGH_LAG = 23.0;
const SYNC_ROUGH_WINDOWS = 3;
// Frames that must be presented after the renderer reports `ready` before
// anything is counted. Start-up is not steady state (the loading screen is
// fading, caches are cold, the wasm engine is still optimizing) and those
// first samples read 2–3 times the settled cost. Counted in frames, not time,
// so a stalled renderer keeps waiting instead of "warming up" through the
// stall.
const WARMUP_FRAMES = 60;
// The refresh period is the typical gap between this thread's animation
// frames: of the last REFRESH_SAMPLES gaps, the mean of those within
// REFRESH_INLIER of the median (a frame this thread itself missed, or a gap
// around a tab switch, is not a refresh). Two seconds of history at 60 Hz, so
// moving the window to a screen with another rate shows within a moment.
const REFRESH_SAMPLES = 120;
const REFRESH_MIN_SAMPLES = 30;
const REFRESH_INLIER = 0.15;
// A window whose frame span is longer than the window itself by more than
// this (ms) contains a pause from before it (a hidden tab, a debugger stop):
// a pause is not missed frames, so the window is not counted.
const PAUSE_SLACK_MS = 250;

// Measures the display's refresh period from this thread's own animation
// frames. This thread does little per frame, so it keeps up with the display
// even when the renderer does not. Returns { periodMs() }: the period in
// milliseconds, or null until enough frames have been seen.
function measureRefresh() {
  const gaps = [];
  let last = null;
  const tick = (t) => {
    if (last !== null) {
      gaps.push(t - last);
      if (gaps.length > REFRESH_SAMPLES) gaps.shift();
    }
    last = t;
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
  // A hidden tab runs no frames: the gap across it is not a refresh.
  document.addEventListener('visibilitychange', () => {
    last = null;
  });
  return {
    periodMs() {
      if (gaps.length < REFRESH_MIN_SAMPLES) return null;
      const sorted = [...gaps].sort((a, b) => a - b);
      const median = sorted[sorted.length >> 1];
      let sum = 0;
      let count = 0;
      for (const gap of gaps) {
        if (Math.abs(gap - median) <= median * REFRESH_INLIER) {
          sum += gap;
          count += 1;
        }
      }
      return count > 0 && sum > 0 ? sum / count : null;
    },
  };
}

// The panel's first lines, which never change, and what follows them until
// there are numbers to show.
const HEAD_MAIN = 'workers\n  main          ui · audio · input\n';
const HEAD_STARTING = `${HEAD_MAIN}  render        starting…\n  physics       starting…`;
const HEAD_WARMING = `${HEAD_MAIN}  render        warming up…\n  physics       warming up…`;

// `getBridge()` returns the current app's Bridge (or null while there is
// none); `getLastCue()` the latest impact cue as { name, intensity }, or null.
// Returns { markReady() }, to call when the renderer reports `ready`.
export function installStats({ getBridge, getLastCue }) {
  const panel = document.getElementById('stats');
  const head = document.getElementById('stats-head');
  const syncLine = document.getElementById('stats-sync');
  const tail = document.getElementById('stats-tail');
  const button = document.getElementById('stats-btn');

  // Off by default: on a phone the panel covers a lot of the screen. A stored
  // "open" is only honoured on a desktop; on a touch device the button still
  // opens it for the session.
  const setOpen = (open) => {
    panel.hidden = !open;
    button.classList.toggle('active', open);
  };
  setOpen(!coarsePointer() && storedBool(STATS_STORAGE_KEY) === true);
  button.addEventListener('click', () => {
    const open = panel.hidden;
    setOpen(open);
    storeBool(STATS_STORAGE_KEY, open);
  });

  const show = (headText, sync = '', bad = false, tailText = '') => {
    head.textContent = headText;
    syncLine.textContent = sync;
    syncLine.classList.toggle('sync-bad', bad);
    tail.textContent = tailText;
  };
  show(HEAD_STARTING);

  const refresh = measureRefresh();
  let renderReady = false;
  let bridge = null; // the Bridge the state below belongs to
  let baseline = null; // the previous sample
  let fpsEma = null;
  let stepsEma = null;
  let frameWorkEma = null;
  let stepWorkEma = null;
  let gpuEma = null;
  // Trusted windows in a row without a single GPU sample: after a few, the
  // device evidently has no timestamp queries, and the line says so.
  let gpuEmptyWindows = 0;
  let roughWindows = 0;
  // Missed refreshes: in the latest window (per second), and the running
  // total. The total is kept signed: a sample that lands between the
  // renderer's two counter writes counts one frame early or late, and the
  // next window evens it out.
  let missedRate = null;
  let missedTotal = 0;
  // frameCount when counting could first start (see WARMUP_FRAMES).
  let warmFrame = null;

  // A hidden tab runs no frames: never turn that gap into a rate.
  document.addEventListener('visibilitychange', () => {
    baseline = null;
  });

  function sample() {
    const now = performance.now();
    const current = getBridge();
    if (current !== bridge) {
      // No app, or a new one after a hot swap. A new app's counters start
      // from zero, so nothing measured against the old one carries over.
      bridge = current;
      baseline = null;
      fpsEma = stepsEma = frameWorkEma = stepWorkEma = gpuEma = null;
      gpuEmptyWindows = 0;
      roughWindows = 0;
      missedRate = null;
      missedTotal = 0;
      warmFrame = null;
    }
    if (!bridge) {
      show(HEAD_STARTING);
      return;
    }

    const stats = bridge.readStats();
    // Read (which resets it) every time, also while warming up, so the
    // start-up spike is gone before the first window that is shown.
    const syncError = bridge.takeSyncError();
    const previous = baseline;
    baseline = { at: now, ...stats };

    // Nothing is counted until the renderer is drawing for real, and then
    // only after WARMUP_FRAMES more frames. The baseline above keeps moving,
    // so the first window shown starts exactly where the warm-up ends.
    if (!renderReady) return; // still "starting…"
    warmFrame ??= stats.frameCount;
    if (((stats.frameCount - warmFrame) >>> 0) < WARMUP_FRAMES) {
      show(HEAD_WARMING);
      return;
    }
    // No trustworthy window (the first sample, or a jittered or throttled
    // gap): the baseline has been reset above; keep the last text up.
    if (!previous || !(now - previous.at >= MIN_WINDOW_MS && now - previous.at < MAX_WINDOW_MS)) {
      return;
    }
    const seconds = (now - previous.at) / 1000;

    const frames = (stats.frameCount - previous.frameCount) >>> 0;
    const steps = (stats.latestStep - previous.latestStep) >>> 0;
    const smooth = (ema, value) => (ema === null ? value : ema + (value - ema) * EMA_ALPHA);
    fpsEma = smooth(fpsEma, frames / seconds);
    stepsEma = smooth(stepsEma, steps / seconds);
    // Average CPU ms per frame and per step over the window. With no frame
    // (or step) to divide by, the previous value stays.
    if (frames > 0) {
      const ms = ((stats.frameWorkUs - previous.frameWorkUs) >>> 0) / frames / 1000;
      frameWorkEma = smooth(frameWorkEma, ms);
    }
    if (steps > 0) {
      const ms = ((stats.stepWorkUs - previous.stepWorkUs) >>> 0) / steps / 1000;
      stepWorkEma = smooth(stepWorkEma, ms);
    }
    // Average GPU ms per SAMPLED frame: the readback skips frames while one
    // is in flight, so this divides by GPU samples, not by frames.
    const gpuFrames = (stats.gpuFrameCount - previous.gpuFrameCount) >>> 0;
    if (gpuFrames > 0) {
      gpuEmptyWindows = 0;
      const ms = ((stats.gpuFrameUs - previous.gpuFrameUs) >>> 0) / gpuFrames / 1000;
      gpuEma = smooth(gpuEma, ms);
    } else {
      gpuEmptyWindows += 1;
    }
    // Refreshes that went by in the window, by the renderer's own frame
    // timestamps, minus the frames it presented in them.
    const periodMs = refresh.periodMs();
    const spanMs = ((stats.frameSpanUs - previous.frameSpanUs) >>> 0) / 1000;
    if (periodMs !== null && spanMs <= now - previous.at + PAUSE_SLACK_MS) {
      const missed = Math.round(spanMs / periodMs) - frames;
      missedTotal += missed;
      missedRate = Math.max(missed, 0) / seconds;
    }
    const displayText = periodMs === null ? 'measuring…' : `${(1000 / periodMs).toFixed(0)} Hz`;
    const missedText =
      missedRate === null
        ? '—'
        : `${missedRate.toFixed(0)}/s · ${Math.max(missedTotal, 0)} total`;

    let gpuText = '—';
    if (gpuEma !== null) gpuText = `${gpuEma.toFixed(1)} ms`;
    else if (gpuEmptyWindows >= 3) gpuText = 'n/a';
    const cpuText = (ema, digits) => (ema === null ? '—' : `${ema.toFixed(digits)} ms`);

    const headText =
      HEAD_MAIN +
      `  display       ${displayText}\n` +
      `  render        ${fpsEma.toFixed(0)} fps\n` +
      `  missed        ${missedText}\n` +
      `  gpu time      ${gpuText}\n` +
      `  cpu time      ${cpuText(frameWorkEma, 1)}\n` +
      `  physics time  ${cpuText(stepWorkEma, 2)}\n` +
      `  physics       ${stepsEma.toFixed(0)} steps/s\n`;

    const lag = stats.displayLag;
    const roughNow = syncError >= SYNC_ROUGH_ERR && lag >= SYNC_ROUGH_LAG;
    roughWindows = roughNow ? roughWindows + 1 : 0;
    const bad = roughWindows >= SYNC_ROUGH_WINDOWS;
    const sync = bad
      ? `  sync          ROUGH ±${syncError.toFixed(1)} · lag ${lag.toFixed(0)}`
      : `  sync          ok · lag ${lag.toFixed(0)}`;

    let tailText = '';
    // 0 from an app built before the field existed: then say nothing.
    const threads = stats.solverThreads;
    if (threads > 0) {
      tailText += `\n  box3d         ${threads} thread${threads === 1 ? '' : 's'}`;
    }
    // Every active pose but slot 0, which is the red ball (APP_PLAYER_SLOT in
    // app/src/app.h).
    tailText += `\n\nballs dropped  ${Math.max(stats.activePoses - 1, 0)}`;
    const cue = getLastCue();
    tailText += `\nlast cue       ${cue ? `${cue.name} ${cue.intensity.toFixed(2)}` : '-'}`;

    show(headText, sync, bad, tailText);
  }

  setInterval(sample, SAMPLE_INTERVAL_MS);

  return {
    markReady() {
      renderReady = true;
    },
  };
}
