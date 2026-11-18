// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// stats.js — the stats panel (top right) and the Stats button (bottom left)
// that shows and hides it.
//
// A frame budget first, then one group per thread, so that every number sits
// under the thread that spent it, and every number says its unit and what it
// is per:
//
//   frame budget               the time one display refresh allows, how much
//                              of it the GPU and each thread use per frame,
//                              and the headroom the largest of them leaves.
//                              CPU and GPU work run in parallel, so each must
//                              fit the budget on its own; they do not add up,
//                              except when the CPU waits on the GPU, which
//                              shows as missed frames.
//   display                    the display's refresh rate (measured here, from
//                              this thread's own animation frames), the frames
//                              the renderer presented per second, and the
//                              refreshes that went by without a new frame.
//   GPU                        the GPU's time per frame, from the renderer's
//                              timestamp queries (`n/a` without them). Its
//                              rows are the passes; on GPUs that run passes
//                              overlapped (Apple's) they add up to more than
//                              the frame.
//   renderer (render worker)   the render worker's CPU time per frame: the
//                              phases of its frame, and inside render() what
//                              awsm-renderer does to prepare, in each pass,
//                              and to submit.
//   app (C, app worker)        the app's own CPU time per fixed step (the
//                              step without Box3D's part), its steps per
//                              second, and the frames the renderer drew
//                              before the app had published the moment they
//                              show ("late": bridge.h, "Frame pacing").
//   physics (Box3D, N threads) the time of Box3D's step, which the app
//                              worker runs together with the solver threads.
//                              At 240 steps a second the two together have
//                              about 4.2 ms; watch this grow as balls pile up.
//   page (main thread)         this thread's own work per frame.
//
// A group's header always shows its headline number. Expanding a group shows
// its breakdown, and only then is the breakdown measured: the page sets a bit
// in the bridge block (`detail`), and the app and the renderer time their
// phases and passes only while it is set. A collapsed panel costs nothing.
//
// Once a second the panel samples the running totals the app and the renderer
// keep in the bridge block (bridge_stats and the timing table in
// app/src/runtime/bridge.h): no messages. Every figure is the change in a
// total divided by the change in what it is per (frames, steps, or REAL
// seconds: a one-second timer is not exactly a second apart, and is throttled
// in a background tab), then smoothed.

import { DETAIL, TIMING } from './bridge.js';
import { STATS_STORAGE_KEY, coarsePointer, storeBool, storedBool } from './settings.js';

const SAMPLE_INTERVAL_MS = 1000;
// Smoothing weight of each new sample: a real change shows within 2–3
// samples, while the readout holds steady against noise.
const EMA_ALPHA = 0.35;
// A sample window is only trusted between these lengths (ms). Shorter: two
// timer callbacks ran almost together, and the quotient is noise. Longer: the
// tab was hidden or throttled, and the window measures the stall.
const MIN_WINDOW_MS = 200;
const MAX_WINDOW_MS = 3000;
// The late-frames line turns red when the app was late in this many sample
// windows in a row: a steady condition, not a one-off such as a tab switch.
const LATE_WINDOWS = 3;
// Frames presented after the renderer reports `ready` before anything is
// counted: start-up (cold caches, the browser still optimizing the wasm) reads
// several times the settled cost. Counted in frames, not time, so a stalled
// renderer keeps waiting instead of "warming up" through the stall.
const WARMUP_FRAMES = 60;
// The refresh period is the mean of the last REFRESH_SAMPLES gaps between
// this thread's animation frames, ignoring gaps more than REFRESH_INLIER away
// from the median (a frame this thread missed, a tab switch).
const REFRESH_SAMPLES = 120;
const REFRESH_MIN_SAMPLES = 30;
const REFRESH_INLIER = 0.15;
// A window whose frame span exceeds the window itself by more than this (ms)
// contains a pause from before it (a hidden tab, a debugger stop), which is
// not missed frames: the window is not counted.
const PAUSE_SLACK_MS = 250;

// The groups, top to bottom. `detail`: the bit that asks the other threads to
// measure this group's breakdown while it is expanded. `lane`: the group's
// colour (style.css, --lane-*), shown as a dot on its header and on its row
// of the frame budget, so that the two pair up at a glance.
const GROUPS = [
  { id: 'budget', label: 'frame budget' },
  { id: 'display', label: 'display' },
  { id: 'gpu', label: 'GPU', detail: DETAIL.GPU, lane: 'gpu' },
  { id: 'renderer', label: 'renderer (render worker)', detail: DETAIL.RENDERER, lane: 'renderer' },
  // The app's own code and Box3D's step both run on the app worker: one
  // lane, so the same colour on both groups.
  { id: 'app', label: 'app (C, app worker)', detail: DETAIL.APP, lane: 'sim' },
  // The label gains the thread count once the app has reported it.
  { id: 'physics', label: 'physics (Box3D)', detail: DETAIL.PHYSICS, lane: 'sim' },
  { id: 'page', label: 'page (main thread)', lane: 'page' },
];
// A lane's share of the frame budget from which its bar warns.
const SHARE_WARN = 0.8;

// The renderer's phase that awsm-renderer's own scopes are a breakdown of,
// and the subgroups those scopes are shown in: by kind, with the prefix the
// renderer gives a kind's names taken off.
const RENDER_PHASE = 'render (encode + submit)';
const SCOPE_GROUPS = [
  { kind: TIMING.RENDERER_PREP, label: 'prepare', prefix: 'Prep: ' },
  { kind: TIMING.RENDERER_PASS, label: 'passes', prefix: '' },
  { kind: TIMING.RENDERER_SUBMIT, label: 'submit', prefix: 'Submit: ' },
  { kind: TIMING.RENDERER_HOOK, label: 'hooks', prefix: '' },
];
// Scopes faster than this (ms) are folded into one "N more" line.
const FOLD_UNDER_MS = 0.001;
// Box3D's stage that the parts of its solver are a breakdown of.
const SOLVE_STAGE = 'solve';

// A duration with its unit: microseconds below a tenth of a millisecond.
function duration(ms) {
  if (ms < 0.09995) {
    const us = ms * 1000;
    return `${us < 9.95 ? us.toFixed(1) : us.toFixed(0)} µs`;
  }
  return `${ms < 9.995 ? ms.toFixed(2) : ms.toFixed(1)} ms`;
}

const smooth = (ema, value) => (ema == null ? value : ema + (value - ema) * EMA_ALPHA);

// For the frame budget's rows: milliseconds to one decimal and a share of the
// budget, neither of which may read as nothing when it is something.
const tenths = (ms) => (ms > 0 && ms < 0.05 ? '<0.1' : ms.toFixed(1));
const share = (ratio) => (ratio > 0 && ratio < 0.005 ? '<1%' : `${Math.round(ratio * 100)}%`);

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

// A lane's dot.
function dot(lane) {
  const element = document.createElement('span');
  element.className = 'stats-dot';
  element.dataset.lane = lane;
  return element;
}

// Fills `element` with a value as it is shown: the numbers bright, and the
// units and words around them smaller and dimmer.
function showValue(element, text) {
  element.replaceChildren(
    ...text
      .split(/(<?\d[\d.]*%?)/)
      .filter((piece) => piece !== '')
      .map((piece) => {
        const span = document.createElement('span');
        span.className = /^<?\d/.test(piece) ? 'stats-num' : 'stats-unit';
        span.textContent = piece;
        return span;
      }),
  );
}

// Builds the panel's elements: per group, a header button (label left,
// headline right) and the rows it shows while expanded. Returns
// { set(id, headline, rows, bad), label(id, text), detail(), onToggle }.
function buildGroups(panel) {
  const parts = new Map();
  const view = { onToggle: () => {} };
  for (const group of GROUPS) {
    const key = `${STATS_STORAGE_KEY}_open_${group.id}`;
    const header = document.createElement('button');
    header.type = 'button';
    header.className = 'stats-group';
    const label = document.createElement('span');
    label.className = 'stats-name';
    const arrow = document.createElement('span');
    const name = document.createTextNode(group.label);
    label.append(arrow, ...(group.lane ? [dot(group.lane)] : []), name);
    const headline = document.createElement('span');
    headline.className = 'stats-value';
    header.append(label, ' ', headline); // the space is for a screen reader
    const rows = document.createElement('div');
    rows.className = 'stats-rows';
    rows.id = `stats-rows-${group.id}`;
    header.setAttribute('aria-controls', rows.id);

    const part = { group, open: false, header, headline, rows, label: group.label };
    part.showLabel = () => {
      arrow.textContent = part.open ? '▾ ' : '▸ ';
      name.textContent = part.label;
    };
    const setOpen = (open) => {
      part.open = open;
      rows.hidden = !open;
      header.setAttribute('aria-expanded', String(open));
      part.showLabel();
    };
    setOpen(storedBool(key) === true); // collapsed unless the user opened it
    header.addEventListener('click', () => {
      setOpen(!part.open);
      storeBool(key, part.open);
      view.onToggle();
    });
    parts.set(group.id, part);
    panel.append(header, rows);
  }

  // `rows`: [label, value] pairs; a third entry indents a row by that many
  // levels, 1 or 2 (a breakdown of the row above), a fourth marks it bad
  // (red), and a fifth, { lane, share }, makes it a lane of the frame budget:
  // the lane's dot, and a bar for its share of the budget.
  view.set = (id, headline, rows = [], bad = false) => {
    const part = parts.get(id);
    showValue(part.headline, headline);
    part.header.classList.toggle('stats-bad', bad);
    if (!part.open) return;
    part.rows.replaceChildren(
      ...rows.map(([label, value, level, rowBad, budget]) => {
        const row = document.createElement('div');
        const nested = ['', ' stats-nested', ' stats-nested stats-nested-2'][Number(level ?? 0)];
        row.className = `stats-row${nested}${rowBad ? ' stats-bad' : ''}`;
        const name = document.createElement('span');
        name.className = 'stats-name';
        name.append(...(budget ? [dot(budget.lane)] : []), label);
        const figure = document.createElement('span');
        figure.className = 'stats-value';
        showValue(figure, value);
        if (budget) {
          // The share as a bar too; its colour says how close to the budget.
          const bar = document.createElement('span');
          const state = budget.share > 1 ? ' stats-over' : budget.share >= SHARE_WARN ? ' stats-warn' : '';
          bar.className = `stats-bar${state}`;
          const fill = document.createElement('span');
          fill.style.width = `${Math.min(budget.share, 1) * 100}%`;
          bar.append(fill);
          figure.prepend(bar);
        }
        row.append(name, figure);
        return row;
      }),
    );
  };
  view.label = (id, text) => {
    const part = parts.get(id);
    if (part.label === text) return;
    part.label = text;
    part.showLabel();
  };
  // The DETAIL bits of the groups that are expanded.
  view.detail = () => {
    let bits = 0;
    for (const part of parts.values()) if (part.open) bits |= part.group.detail ?? 0;
    return bits;
  };
  return view;
}

// `getBridge()` returns the current app's Bridge (or null while there is
// none); `getLastCue()` the latest cue game.js showed as { name, intensity },
// or null; `gameRows(activePoses)` the game's own rows for the app group
// (dom/game.js); `onHelp()` opens the explanation of the panel (About's
// Stats tab).
// Returns { markReady(), pageFrame(ms) }: call the first when the renderer
// reports `ready`, the second once per animation frame with the time this
// thread spent on it.
export function installStats({ getBridge, getLastCue, gameRows, onHelp }) {
  const panel = document.getElementById('stats');
  const button = document.getElementById('stats-btn');
  const help = document.createElement('button');
  help.type = 'button';
  help.className = 'stats-help';
  help.textContent = 'what do these mean?';
  help.addEventListener('click', onHelp);
  panel.append(help);
  const view = buildGroups(panel);

  const refresh = measureRefresh();
  let renderReady = false;
  let bridge = null; // the Bridge the state below belongs to
  let baseline = null; // the previous sample
  let fpsEma = null;
  let stepsEma = null;
  let frameWorkEma = null;
  let stepWorkEma = null; // the app's own part of a step
  let physicsEma = null; // Box3D's part
  let appFrameEma = null; // the app worker's steps (both parts), per frame drawn
  let activePoses = 0;
  let gpuEma = null;
  let pageEma = null;
  // Trusted windows in a row without a single GPU sample: after a few, the
  // device evidently has no timestamp queries, and the group says so.
  let gpuEmptyWindows = 0;
  // Frames the app was late for: the total since counting started, and how
  // many windows in a row had any.
  let lateTotal = 0;
  let lateWindows = 0;
  // Missed refreshes: in the latest window (per second), and the running
  // total. The total is kept signed: a sample that lands between the
  // renderer's two counter writes counts one frame early or late, and the
  // next window evens it out.
  let missedRate = null;
  let missedTotal = 0;
  // frameCount when counting could first start (see WARMUP_FRAMES).
  let warmFrame = null;
  // The timing table: each entry's previous reading and its smoothed mean,
  // by entry.
  let timingBaseline = new Map();
  const timingMeans = new Map();
  // This thread's own work, summed between samples.
  let pageMs = 0;
  let pageFrames = 0;

  // The breakdowns are only measured while the panel shows them: tell the
  // other threads which groups are expanded (none while the panel is hidden).
  const publishDetail = () => getBridge()?.setDetail(panel.hidden ? 0 : view.detail());
  view.onToggle = () => {
    publishDetail();
    render(); // the rows of a group just opened fill in on the next sample
  };

  // Off by default: on a phone the panel covers a lot of the screen. A stored
  // "open" is only honoured on a desktop; on a touch device the button still
  // opens it for the session.
  const setOpen = (open) => {
    panel.hidden = !open;
    button.classList.toggle('active', open);
    publishDetail();
  };
  setOpen(!coarsePointer() && storedBool(STATS_STORAGE_KEY) === true);
  button.addEventListener('click', () => {
    const open = panel.hidden;
    setOpen(open);
    storeBool(STATS_STORAGE_KEY, open);
  });

  // A hidden tab runs no frames: never turn that gap into a rate.
  document.addEventListener('visibilitychange', () => {
    baseline = null;
  });

  // What the panel shows until there are numbers.
  let placeholder = 'starting…';
  let late = { rate: 0, bad: false };

  // The timing-table rows of one kind: [name, mean in ms], in table order.
  const timings = (kind) =>
    [...timingMeans.values()].filter((entry) => entry.kind === kind && entry.ms != null);
  const slowestFirst = (entries) => entries.sort((a, b) => b.ms - a.ms);

  function render() {
    if (placeholder) {
      for (const group of GROUPS) view.set(group.id, group.id === 'display' ? '—' : placeholder);
      return;
    }
    const periodMs = refresh.periodMs();
    const hz = periodMs === null ? null : `${(1000 / periodMs).toFixed(0)} Hz`;
    const fps = `${fpsEma.toFixed(0)}`;
    view.set('display', `${hz ?? 'measuring…'} · ${fps} fps`, [
      ['refresh rate', hz ?? 'measuring…'],
      ['frames presented', `${fps} / s`],
      [
        'missed frames',
        missedRate === null ? '—' : `${missedRate.toFixed(0)} / s · ${Math.max(missedTotal, 0)} total`,
      ],
    ]);

    // The frame budget: one lane for the GPU and one per thread, each with
    // what it uses per frame. They run in parallel, so each must fit the
    // budget on its own, and the headroom is what the largest leaves.
    const lanes = [
      ['GPU', gpuEma, 'gpu'],
      ['renderer (render worker)', frameWorkEma, 'renderer'],
      ['app worker (app + Box3D step)', appFrameEma, 'sim'],
      ['page (main thread)', pageEma, 'page'],
    ].filter(([, ms]) => ms !== null);
    if (periodMs === null || lanes.length === 0) {
      view.set('budget', 'measuring…');
    } else {
      const budget = periodMs.toFixed(1);
      const headroom = periodMs - Math.max(...lanes.map(([, ms]) => ms));
      const rows = lanes.map(([label, ms, lane]) => [
        label,
        `${tenths(ms)} / ${budget} ms (${share(ms / periodMs)})`,
        0,
        false,
        { lane, share: ms / periodMs },
      ]);
      if (gpuEma !== null && frameWorkEma !== null) {
        rows.push([
          'latency (renderer + GPU), reference only',
          `${(frameWorkEma + gpuEma).toFixed(1)} ms`,
        ]);
      }
      const left =
        headroom < 0 ? `over by ${(-headroom).toFixed(1)} ms` : `headroom ${headroom.toFixed(1)} ms`;
      view.set('budget', `${budget} ms (${hz}) · ${left}`, rows, headroom < 0);
    }
    let gpu = '—';
    if (gpuEma !== null) gpu = `${duration(gpuEma)} / frame`;
    else if (gpuEmptyWindows >= 3) gpu = 'n/a';
    view.set(
      'gpu',
      gpu,
      slowestFirst(timings(TIMING.GPU_PASS)).map((pass) => [pass.name, `${duration(pass.ms)} / frame`]),
    );

    // The renderer's phases in the order they run. Under render(), what
    // awsm-renderer's own scopes say it spends, in subgroups by kind, and
    // what is left over: the glue between them.
    const rendererRows = [];
    const perFrame = (ms) => `${duration(ms)} / frame`;
    const total = (entries) => entries.reduce((sum, entry) => sum + entry.ms, 0);
    for (const phase of timings(TIMING.RENDERER_PHASE)) {
      rendererRows.push([phase.name, perFrame(phase.ms)]);
      if (phase.name !== RENDER_PHASE) continue;
      let inScopes = 0;
      let subgroups = 0;
      for (const { kind, label, prefix } of SCOPE_GROUPS) {
        const scopes = slowestFirst(timings(kind));
        if (scopes.length === 0) continue;
        subgroups += 1;
        inScopes += total(scopes);
        rendererRows.push([label, perFrame(total(scopes)), 1]);
        for (const scope of scopes.filter((scope) => scope.ms >= FOLD_UNDER_MS)) {
          const name = scope.name.startsWith(prefix) ? scope.name.slice(prefix.length) : scope.name;
          rendererRows.push([name, perFrame(scope.ms), 2]);
        }
        const small = scopes.filter((scope) => scope.ms < FOLD_UNDER_MS);
        if (small.length > 0) {
          rendererRows.push([`${small.length} more under 1 µs`, perFrame(total(small)), 2]);
        }
      }
      if (subgroups > 0) {
        rendererRows.push(['outside the scopes', perFrame(Math.max(phase.ms - inScopes, 0)), 1]);
      }
    }
    view.set(
      'renderer',
      frameWorkEma === null ? '—' : `${duration(frameWorkEma)} / frame`,
      rendererRows,
    );

    const cue = getLastCue();
    view.set(
      'app',
      stepWorkEma === null ? '—' : `${duration(stepWorkEma)} / step`,
      [
        ['steps', `${stepsEma.toFixed(0)} steps/s`],
        ['late frames', `${late.rate.toFixed(0)} / s · ${lateTotal} total`, false, late.bad],
        ...timings(TIMING.APP_PHASE).map((phase) => [phase.name, `${duration(phase.ms)} / step`]),
        ...gameRows(activePoses),
        ['last cue', cue ? `${cue.name} ${cue.intensity.toFixed(2)}` : '—'],
      ],
      late.bad,
    );

    // Box3D's stages in the order they run, with the parts of its solver
    // under the solve stage.
    const physicsRows = [];
    const solverParts = timings(TIMING.PHYSICS_SOLVE);
    for (const stage of timings(TIMING.PHYSICS_STAGE)) {
      physicsRows.push([stage.name, `${duration(stage.ms)} / step`]);
      if (stage.name !== SOLVE_STAGE) continue;
      for (const part of solverParts) physicsRows.push([part.name, `${duration(part.ms)} / step`, true]);
    }
    view.set('physics', physicsEma === null ? '—' : `${duration(physicsEma)} / step`, physicsRows);

    // All this thread does per frame is in main.js's frame(): one row.
    const page = pageEma === null ? '—' : `${duration(pageEma)} / frame`;
    view.set('page', page, [['frame marks + sound cues', page]]);
  }

  function sample() {
    const now = performance.now();
    const current = getBridge();
    if (current !== bridge) {
      // No app, or a new one after a hot swap. A new app's counters start
      // from zero, so nothing measured against the old one carries over.
      bridge = current;
      baseline = null;
      fpsEma = stepsEma = frameWorkEma = stepWorkEma = physicsEma = gpuEma = null;
      appFrameEma = null;
      gpuEmptyWindows = 0;
      lateTotal = 0;
      lateWindows = 0;
      missedRate = null;
      missedTotal = 0;
      warmFrame = null;
      timingBaseline = new Map();
      timingMeans.clear();
      publishDetail(); // the new block has not been told yet
    }
    if (!bridge) {
      placeholder = 'starting…';
      render();
      return;
    }

    const stats = bridge.readStats();
    // Threads Box3D steps the world on, counting the app's (0: an app from
    // before it reported them).
    const threads = stats.solverThreads;
    if (threads > 0) {
      view.label('physics', `physics (Box3D, ${threads} thread${threads === 1 ? '' : 's'})`);
    }
    const previous = baseline;
    baseline = { at: now, ...stats };
    const previousTimings = timingBaseline;
    timingBaseline = new Map(bridge.readTimings().map((entry) => [entry.slot, entry]));
    const page = { ms: pageMs, frames: pageFrames };
    pageMs = 0;
    pageFrames = 0;

    // Nothing is counted until the renderer is drawing for real, and then
    // only after WARMUP_FRAMES more frames. The baseline above keeps moving,
    // so the first window shown starts exactly where the warm-up ends.
    if (!renderReady) return; // still "starting…"
    warmFrame ??= stats.frameCount;
    if (((stats.frameCount - warmFrame) >>> 0) < WARMUP_FRAMES) {
      placeholder = 'warming up…';
      render();
      return;
    }
    // No trustworthy window (the first sample, or a jittered or throttled
    // gap): the baseline has been reset above; keep the last figures up.
    if (!previous || !(now - previous.at >= MIN_WINDOW_MS && now - previous.at < MAX_WINDOW_MS)) {
      return;
    }
    placeholder = null;
    const seconds = (now - previous.at) / 1000;

    const frames = (stats.frameCount - previous.frameCount) >>> 0;
    const steps = (stats.latestStep - previous.latestStep) >>> 0;
    fpsEma = smooth(fpsEma, frames / seconds);
    stepsEma = smooth(stepsEma, steps / seconds);
    // Average CPU ms per frame and per step over the window. With no frame
    // (or step) to divide by, the previous value stays.
    if (frames > 0) {
      const ms = ((stats.frameWorkUs - previous.frameWorkUs) >>> 0) / frames / 1000;
      frameWorkEma = smooth(frameWorkEma, ms);
    }
    // The step in two parts: Box3D's, and the rest, which is the app's own.
    // (An app from before the split reports no physics time: all is the app's.)
    const stepWorkMs = ((stats.stepWorkUs - previous.stepWorkUs) >>> 0) / 1000;
    if (steps > 0) {
      const stepMs = stepWorkMs / steps;
      const physicsMs = ((stats.physicsStepUs - previous.physicsStepUs) >>> 0) / steps / 1000;
      stepWorkEma = smooth(stepWorkEma, Math.max(stepMs - physicsMs, 0));
      physicsEma = smooth(physicsEma, physicsMs);
    }
    // The whole step per frame drawn (time per step × steps per frame): what
    // the app worker uses of a frame's budget. Box3D's part is in it: the
    // app worker runs the step, and its solver threads only help it.
    if (frames > 0) appFrameEma = smooth(appFrameEma, stepWorkMs / frames);
    activePoses = stats.activePoses;
    if (page.frames > 0) pageEma = smooth(pageEma, page.ms / page.frames);
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

    const lateFrames = (stats.appLate - previous.appLate) >>> 0;
    lateTotal += lateFrames;
    lateWindows = lateFrames > 0 ? lateWindows + 1 : 0;
    late = { rate: lateFrames / seconds, bad: lateWindows >= LATE_WINDOWS };

    // The breakdowns: each entry's mean over the window. An entry that did
    // not advance is no longer being measured (its group was collapsed, or
    // the pass did not run), so it drops out rather than show a stale figure.
    for (const [slot, entry] of timingBaseline) {
      const before = previousTimings.get(slot);
      const count = before ? (entry.count - before.count) >>> 0 : 0;
      if (count === 0) {
        timingMeans.delete(slot);
        continue;
      }
      const ms = ((entry.totalNs - before.totalNs) >>> 0) / count / 1e6;
      const mean = timingMeans.get(slot);
      timingMeans.set(slot, { kind: entry.kind, name: entry.name, ms: smooth(mean?.ms, ms) });
    }

    render();
  }

  render();
  setInterval(sample, SAMPLE_INTERVAL_MS);

  return {
    markReady() {
      renderReady = true;
    },
    pageFrame(ms) {
      pageMs += ms;
      pageFrames += 1;
    },
  };
}
