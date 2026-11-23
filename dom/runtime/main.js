// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// main.js — the main thread. Owns the page: the canvas, input, the HUD, sound,
// and the two workers. No framework, no build step. Everything about your
// game (its name, keys, HUD words, what its cues sound like) comes from
// dom/game.js.
//
//   renderer/worker.js  draws into the canvas (transferred to it as an
//                       OffscreenCanvas) from the snapshots the app publishes
//   app/worker.js       runs the C app (your game and its runtime); all logic
//                       and state live there
//   audio/audio.js      the sound, a Rust module that runs here on the page
//                       (WebAudio exists only on the main thread); it plays
//                       the awsm-audio project in audio-project/
//
// The app's memory holds the shared "bridge block" (see
// app/src/runtime/bridge.h).
// When the app worker reports `ready`, this thread forwards its memory to the
// renderer (`attach`) and keeps its own view (bridge.js) to write input and
// read cues and stats. After that, nothing per-frame goes through postMessage;
// the only messages are rare ones, such as a Settings change.
//
// The panels over the canvas each have a module of their own:
//
//   loading.js    the loading screen and the "cannot start" screen
//   settings.js   resolution and anti-aliasing (the Settings button), and
//                 the canvas size that follows from them
//   stats.js      the stats panel (the Stats button)
//   about.js      the About button
//
// This file wires them to the workers, and owns the input and the sound.

import {
  GAME_NAME,
  HUD_CONTROLS,
  KEY_CODES,
  PLAYING_STATUS,
  gameStatsRows,
  onGameCue,
  onSceneGone,
  onSoundLoaded,
} from '../game.js';
import { BUTTON_CANCELLED, Bridge, POINTER, RECORD, frameClockUs } from './bridge.js';
import { installAbout } from './about.js';
import { fatal, loadingDone, loadingLog, loadingStart } from './loading.js';
import { Settings, resetSettingsIfAsked } from './settings.js';
import { installStats } from './stats.js';

const canvas = document.getElementById('view');
const hud = document.getElementById('hud');
const errorPanel = document.getElementById('error');
const errorText = document.getElementById('error-text');

// ── HUD ───────────────────────────────────────────────────────────────────
//
// Top left: the game's name and controls (game.js), what the sound is doing,
// and one status line. The status line goes booting… → loading scene… →
// playing, and shows the latest error while there is one.

const HUD_TEXT = `${GAME_NAME}\n${HUD_CONTROLS}\n`;

// What the status line calls each source of errors.
const ERROR_NAMES = { page: 'page', renderer: 'render', app: 'app' };

let status = 'booting…';

function setStatus(text) {
  status = text;
  renderHud();
}

function renderHud() {
  let line = status;
  const latest = [...errors].at(-1);
  if (latest) {
    const [source, messages] = latest;
    line = `${ERROR_NAMES[source] ?? source} error: ${messages.at(-1).split('\n')[0]}`;
  }
  hud.textContent = `${HUD_TEXT}sound: ${AUDIO_STATES[audioState]}\n${line}`;
}

// ── Errors ────────────────────────────────────────────────────────────────

// Messages by source ('page', 'renderer', 'app'), so replacing the app worker
// can clear the old app's errors without hiding the renderer's. The latest one
// also shows on the HUD's status line and in the loading log; the panel at the
// bottom has them all in full.
const errors = new Map();

function showError(source, message) {
  console.error(`[${source}]`, message);
  loadingLog(`ERROR: ${message}`);
  const list = errors.get(source) ?? [];
  list.push(message);
  errors.delete(source); // re-inserted last: the newest source shows on the HUD
  errors.set(source, list);
  renderErrors();
}

function clearErrors(source) {
  errors.delete(source);
  renderErrors();
}

function renderErrors() {
  const lines = [];
  for (const [source, list] of errors) {
    for (const message of list) lines.push(`${source}: ${message}`);
  }
  errorText.textContent = lines.join('\n\n');
  errorPanel.hidden = lines.length === 0;
  renderHud();
}

// ── Sound ─────────────────────────────────────────────────────────────────

// The sound is optional. It loads alongside the workers, and if anything about
// it fails (a missing file, no WebAudio, a crash inside the module) the page
// says so in the HUD and the console and carries on silently. Nothing else
// waits for it.
//
// It is loaded with a dynamic import() for that reason: a static import of a
// missing dist/audio/audio.js would stop this whole script from running.

// What the HUD says about the sound in each state.
const AUDIO_STATES = {
  loading: 'loading',
  ready: 'starts on a key or click',
  on: 'on',
  failed: 'off (see console)',
};

let audio = null; // the loaded `Audio` (audio/src/lib.rs); null while unavailable
let audioState = 'loading'; // for the HUD: a key of AUDIO_STATES (set with setAudioState)

async function loadAudio(baseUrl) {
  try {
    // Relative to this module (dist/runtime/), so one level up.
    const module = await import('../audio/audio.js');
    await module.default(); // instantiate the wasm
    const loaded = await module.Audio.load(baseUrl);
    onSoundLoaded(loaded); // game.js brings it up to date
    audio = loaded;
    setAudioState('ready');
    loadingLog('audio project loaded');
  } catch (err) {
    audioFailed(err);
  }
}

function audioFailed(err) {
  audio = null;
  setAudioState('failed');
  console.error('[audio] sound is off:', err);
}

function setAudioState(state) {
  if (state === audioState) return;
  audioState = state;
  renderHud();
}

// Browsers only allow audio after a user gesture, so this runs on every key
// press and pointer press. The first one after loading starts the sound;
// later ones are cheap (and re-resume the context if a gesture did not count).
function startAudio() {
  if (!audio) return; // still loading: a later gesture starts it
  try {
    audio.start();
    setAudioState('on');
  } catch (err) {
    audioFailed(err);
  }
}

// What game.js's cue handlers get to use: the sound, the stats panel's
// "last cue" line, and a frame count.
const page = {
  get audio() {
    return audio;
  },
  soundFailed: audioFailed,
  showCue(name, intensity) {
    lastCue = { name, intensity };
  },
  frame: 0,
};

// ── Start ─────────────────────────────────────────────────────────────────

function main() {
  renderHud();
  if (!window.crossOriginIsolated) {
    // In a release build, the service-worker shim reloads the page once it is
    // installed; this first visit is expected to be un-isolated.
    const shimReloading =
      document.querySelector('script[src$="coi-serviceworker.js"]') !== null &&
      'serviceWorker' in navigator &&
      !navigator.serviceWorker.controller &&
      window.isSecureContext;
    if (shimReloading) {
      loadingLog('enabling cross-origin isolation (the page reloads once)…');
      return;
    }
    // There is nothing to fall back to: without isolation there is no shared
    // memory, and so no bridge block. Say why, rather than failing later
    // with an opaque error.
    fatal(
      'Cross-origin isolation is OFF — multithreading is unavailable, so this build cannot ' +
        'run. It needs COOP: same-origin + COEP: require-corp (sent by `task dev` and ' +
        '`task serve`, or re-imposed by coi-serviceworker.js on static hosts like GitHub ' +
        'Pages: `task build` includes a shim for that unless COI=0).',
    );
    return;
  }
  if (!('transferControlToOffscreen' in HTMLCanvasElement.prototype)) {
    fatal(
      'This browser cannot hand a canvas to a worker (OffscreenCanvas), which the renderer needs.',
    );
    return;
  }
  if (!('gpu' in navigator)) {
    fatal('This browser does not support WebGPU, which the renderer needs.');
    return;
  }

  // Before anything reads a stored setting.
  resetSettingsIfAsked();
  loadingLog('main thread booting');

  // dist/ root, so the renderer can resolve asset URLs from its worker.
  const baseUrl = new URL('./', document.baseURI).href;

  // The canvas's backing-store size, from the resolution setting. Set before
  // the canvas is transferred; after that only the renderer can change it.
  const settings = new Settings(canvas);
  let size = settings.backing();
  canvas.width = size.width;
  canvas.height = size.height;
  const offscreen = canvas.transferControlToOffscreen();

  // ── Renderer worker ──

  loadingLog('spawning render worker…');
  const renderer = new Worker('renderer/worker.js', { type: 'module' });
  renderer.addEventListener('message', (event) => {
    const msg = event.data;
    switch (msg?.type) {
      case 'progress':
        loadingLog(msg.message);
        break;
      case 'gpu-info':
        settings.gpuInfo(msg);
        break;
      // The scene is being loaded again (the app asked: R, or a hot swap).
      // The same loading screen as at start-up, until the next `ready`.
      case 'loading':
        loadingStart();
        // No cue comes until the new scene is up: game.js stops whatever
        // would keep sounding.
        onSceneGone(page);
        lastCue = null;
        setStatus('loading scene…');
        break;
      // The scene is on screen: after start-up, and after every reload.
      case 'ready':
        loadingLog('first frames rendered — ready');
        loadingDone();
        stats.markReady();
        settings.ready();
        setStatus(PLAYING_STATUS);
        break;
      case 'aa-compile-start':
        settings.compileStart(msg);
        break;
      case 'aa-compile-progress':
        settings.compileProgress(msg.message);
        break;
      case 'aa-compile-done':
        settings.compileDone();
        break;
      case 'error':
        showError('renderer', msg.message);
        break;
      default:
        console.warn('[renderer] unexpected message', msg);
    }
  });
  renderer.addEventListener('error', (event) => {
    showError('renderer', event.message || 'the worker failed to load (see the console)');
  });
  // The anti-aliasing comes with `init`, so the renderer is built with it
  // instead of recompiling right after start-up.
  const { msaa, smaa } = settings.antiAlias();
  const { width, height } = size;
  renderer.postMessage({ type: 'init', canvas: offscreen, baseUrl, width, height, msaa, smaa }, [
    offscreen,
  ]);

  // ── App worker ──
  //
  // `app` is the current worker plus its bridge view (null until `ready`).
  // Replaced wholesale by swapApp().

  let app = null;

  function spawnApp() {
    loadingLog('spawning app worker (C + Box3D)…');
    const entry = { worker: new Worker('app/worker.js', { type: 'module' }), bridge: null };
    app = entry;
    entry.worker.addEventListener('message', (event) => {
      if (app !== entry) return; // from a worker that has since been replaced
      const msg = event.data;
      if (msg?.type === 'ready') {
        try {
          entry.bridge = new Bridge(msg.memory, msg.bridgePtr);
        } catch (err) {
          showError('app', err.message);
          return;
        }
        const threads = entry.bridge.readStats().solverThreads;
        loadingLog(`app ready — Box3D on ${threads} thread${threads === 1 ? '' : 's'}`);
        entry.bridge.setCanvasSize(size.width, size.height);
        renderer.postMessage({ type: 'attach', memory: msg.memory, bridgePtr: msg.bridgePtr });
      } else if (msg?.type === 'error') {
        showError('app', msg.message);
      }
    });
    entry.worker.addEventListener('error', (event) => {
      if (app !== entry) return;
      showError('app', event.message || 'the worker failed to load (see the console)');
    });
  }

  // Hot swap: replace the app worker with a freshly loaded one (the dev
  // server's reload client calls this after an app rebuild). The renderer
  // keeps showing the last frame, then resets its scene for the new app.
  function swapApp() {
    if (app) {
      app.worker.terminate();
      renderer.postMessage({ type: 'detach' });
    }
    app = null;
    // The new app starts with nothing held or pressed. A key that is still
    // physically down reaches it through the keyboard's auto-repeat.
    heldKeys.clear();
    drag = null;
    // The new app sends no cue until its scene is up: game.js stops
    // whatever would keep sounding.
    onSceneGone(page);
    clearErrors('app');
    spawnApp();
  }

  // ── Panels ──

  const about = installAbout();
  const stats = installStats({
    getBridge: () => app?.bridge ?? null,
    getLastCue: () => lastCue,
    gameRows: gameStatsRows,
    onHelp: () => about.open('stats'),
  });

  // ── Settings: canvas size and anti-aliasing ──

  settings.start({
    // A new backing-store size: the window was resized, or the resolution
    // setting changed. The renderer owns the canvas now, so it resizes it;
    // the app gets the size for its aspect ratio.
    onResize(next) {
      if (next.width === size.width && next.height === size.height) return;
      size = next;
      renderer.postMessage({ type: 'resize', width: size.width, height: size.height });
      app?.bridge?.setCanvasSize(size.width, size.height);
    },
    onAntiAlias({ msaa, smaa }) {
      renderer.postMessage({ type: 'anti-alias', msaa, smaa });
    },
  });

  // ── Keyboard ──
  //
  // The page only reports which of game.js's KEY_CODES go down and up
  // (records on the dom → app ring); what they do is decided in the game.

  // The keys the current app has been told are down.
  const heldKeys = new Set();

  // Take the keyboard from the start, so W/A/S/D work without a click first.
  // In an iframe the browser ignores this without a user gesture: then the
  // page around it has to focus the frame (awsm.fun does, once it loads), or
  // the first press on the canvas takes the keyboard (see pointerdown).
  window.focus();

  window.addEventListener('keydown', (event) => {
    startAudio(); // any key press is a user gesture
    if (event.ctrlKey || event.metaKey || event.altKey) return; // leave shortcuts alone
    const key = KEY_CODES[event.code];
    if (key === undefined) return;
    // A button reached with the keyboard keeps the keys that press it. (One
    // that was clicked does not: `:focus-visible` is the browser's own test
    // for "focused from the keyboard", so the mouse never takes these keys
    // away from the game.)
    if (pressesFocusedControl(event)) return;
    event.preventDefault(); // no page scrolling on Space / arrows
    // A held key repeats its keydown; only the first one is news. While no
    // app is listening nothing is remembered, so a key held through loading
    // is still reported (by its next repeat) once the app is up.
    const bridge = app?.bridge;
    if (!bridge || heldKeys.has(key)) return;
    heldKeys.add(key);
    bridge.setKeyHeld(key, true);
    bridge.sendKey(key, true);
  });

  window.addEventListener('keyup', (event) => {
    const key = KEY_CODES[event.code];
    if (key === undefined || !heldKeys.has(key)) return;
    heldKeys.delete(key);
    app?.bridge?.setKeyHeld(key, false);
    app?.bridge?.sendKey(key, false);
  });

  // Keys released while the page is not focused never send keyup.
  function releaseAllKeys() {
    for (const key of heldKeys) {
      app?.bridge?.setKeyHeld(key, false);
      app?.bridge?.sendKey(key, false);
    }
    heldKeys.clear();
  }
  window.addEventListener('blur', releaseAllKeys);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) releaseAllKeys();
  });

  // Leaving the page (a link, the back button) can keep it in the browser's
  // back/forward cache instead of unloading it, and its workers go on running
  // there: the renderer keeps drawing frames nobody sees, on the GPU the next
  // page needs. A page put in that cache stops its workers, and one brought
  // back from it starts again from the top.
  window.addEventListener('pagehide', (event) => {
    if (!event.persisted) return;
    renderer.terminate();
    app?.worker.terminate();
  });
  window.addEventListener('pageshow', (event) => {
    if (event.persisted) window.location.reload();
  });

  // ── Pointer ──
  //
  // The page only reports what happened, with what kind of device and
  // when. What it means (in the demo: a click that drops a ball, a
  // right-button drag that orbits, a touch swipe that throws) is decided in
  // the game. Pointer
  // capture keeps a drag going when it leaves the canvas.

  let drag = null; // { id, x, y } of the pressed pointer, in CSS pixels

  // Client (CSS pixel) coordinates → normalized device coordinates: x right,
  // y up, both -1..1 across the canvas.
  function pointerKind(event) {
    if (event.pointerType === 'touch') return POINTER.TOUCH;
    if (event.pointerType === 'pen') return POINTER.PEN;
    return POINTER.MOUSE;
  }

  function toNdc(clientX, clientY) {
    const rect = canvas.getBoundingClientRect();
    const x = ((clientX - rect.left) / rect.width) * 2 - 1;
    const y = 1 - ((clientY - rect.top) / rect.height) * 2;
    return [x, y];
  }

  canvas.addEventListener('pointerdown', (event) => {
    if (!event.isPrimary || drag) return; // one pointer at a time
    event.preventDefault();
    // Cancelling the press also cancels the focus change it would bring, so
    // focus is taken by hand. That matters inside an iframe (the awsm.fun
    // gallery shows the page in one): until the frame has focus, the
    // keyboard goes to the page around it.
    canvas.focus({ preventScroll: true });
    startAudio();
    // Capture can only fail for a pointer the browser does not know (events
    // made by a test script); the press itself still counts.
    try {
      canvas.setPointerCapture(event.pointerId);
    } catch {}
    drag = { id: event.pointerId, x: event.clientX, y: event.clientY };
    const [x, y] = toNdc(event.clientX, event.clientY);
    app?.bridge?.setPointer(x, y, event.buttons);
    app?.bridge?.sendPointerDown(event.button, x, y, pointerKind(event), event.timeStamp);
  });

  canvas.addEventListener('pointermove', (event) => {
    if (!event.isPrimary) return;
    const [x, y] = toNdc(event.clientX, event.clientY);
    app?.bridge?.setPointer(x, y, event.buttons);
    // Movement events only while pressed: hovering needs no events (the
    // position above is enough), and it keeps the ring quiet.
    if (drag && event.pointerId === drag.id) {
      const dx = event.clientX - drag.x;
      const dy = event.clientY - drag.y;
      drag.x = event.clientX;
      drag.y = event.clientY;
      app?.bridge?.sendPointerMove(dx, dy, x, y, event.buttons, pointerKind(event), event.timeStamp);
    }
  });

  // A press ends with pointerup, or with pointercancel when the browser takes
  // the pointer away (a touch that became a scroll, a system gesture). The app
  // is told either way, so it never waits for a release that will not come. A
  // cancelled event carries no usable button or position: it is reported with
  // BUTTON_CANCELLED at the last place the pointer was seen.
  function endPress(event) {
    if (!drag || event.pointerId !== drag.id) return;
    const cancelled = event.type === 'pointercancel';
    const [x, y] = cancelled ? toNdc(drag.x, drag.y) : toNdc(event.clientX, event.clientY);
    drag = null;
    app?.bridge?.setPointer(x, y, cancelled ? 0 : event.buttons);
    app?.bridge?.sendPointerUp(
      cancelled ? BUTTON_CANCELLED : event.button,
      x,
      y,
      pointerKind(event),
      event.timeStamp,
    );
  }
  canvas.addEventListener('pointerup', endPress);
  canvas.addEventListener('pointercancel', endPress);

  canvas.addEventListener(
    'wheel',
    (event) => {
      event.preventDefault(); // zoom the scene, not the page
      let dy = event.deltaY;
      if (event.deltaMode === WheelEvent.DOM_DELTA_LINE) dy *= 16;
      else if (event.deltaMode === WheelEvent.DOM_DELTA_PAGE) dy *= canvas.clientHeight;
      app?.bridge?.sendWheel(dy);
    },
    { passive: false },
  );

  canvas.addEventListener('contextmenu', (event) => event.preventDefault());

  // ── Per frame: the frame mark, then cues ──

  function frame(timestamp) {
    const start = performance.now();
    const bridge = app?.bridge;
    if (bridge) {
      // Close this frame's input (if it had any). The browser delivers a
      // frame's input events before its animation frame callbacks, so
      // everything sent so far belongs to this frame or an earlier one. The
      // app applies input one page frame per burst by these marks, so that
      // each frame it draws carries exactly one frame of input
      // (BRIDGE_EV_FRAME_MARK in bridge.h).
      bridge.markFrame(frameClockUs(timestamp));
      // This is how the game tells the page something happened; game.js's
      // onGameCue decides what the page does about it.
      page.frame += 1;
      bridge.drain((record) => {
        if (record.type !== RECORD.CUE) return; // a record type this page does not know
        onGameCue(record, page);
      });
    }
    // For the stats panel: what this thread spends per frame.
    stats.pageFrame(performance.now() - start);
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);

  // ── Go ──

  window.__awsm = {
    swapApp,
    get bridge() {
      return app?.bridge ?? null;
    },
  };
  spawnApp();
  loadAudio(baseUrl); // in parallel with the workers; never throws
  setStatus('loading scene…');
}

// Enter or Space on a control that has the keyboard's focus: the browser's
// to handle (it presses the control), not the game's.
function pressesFocusedControl(event) {
  const pressing = event.code === 'Enter' || event.code === 'NumpadEnter' || event.code === 'Space';
  return (
    pressing &&
    event.target instanceof Element &&
    event.target.matches('button:focus-visible, select:focus-visible, a[href]:focus-visible')
  );
}

// The latest cue game.js showed, for the stats panel: { name, intensity }.
let lastCue = null;

try {
  main();
} catch (err) {
  showError('page', err instanceof Error ? err.stack || err.message : String(err));
}
