// YOUR GAME: start here
//
// game.js — your game's side of the page: its name, the keys it listens to,
// the HUD's words, and what the page does with the game's cues (here: sound).
// The runtime (dom/runtime/main.js) owns the canvas, the workers, the input
// plumbing, loading the sound and the panels, and reads everything it needs
// to know about your game from the exports below.

import { KEY } from './runtime/bridge.js';

// The name the HUD and the "cannot start" screen go by.
export const GAME_NAME = 'physics-multithreaded-c';

// Every localStorage key the page uses starts with this prefix (settings,
// the stats panel). localStorage is shared by everything on the same origin
// (every project under one domain, every app you run on localhost:8000), so
// CHANGE IT when you start your own project from this template, or two
// projects will overwrite each other's settings.
export const STORAGE_PREFIX = 'awsm_physics_mt_c_';

// KeyboardEvent.code → bridge key (BRIDGE_KEY_* in app/src/runtime/bridge.h).
// Only these keys reach the game; what they do is decided in
// app/src/game/game.c. Codes name physical keys, so W/A/S/D sit in the same
// place on every keyboard layout.
export const KEY_CODES = {
  KeyW: KEY.W,
  KeyA: KEY.A,
  KeyS: KEY.S,
  KeyD: KEY.D,
  ArrowUp: KEY.UP,
  ArrowDown: KEY.DOWN,
  ArrowLeft: KEY.LEFT,
  ArrowRight: KEY.RIGHT,
  Space: KEY.SPACE,
  ShiftLeft: KEY.SHIFT,
  ShiftRight: KEY.SHIFT,
  Enter: KEY.ENTER,
  NumpadEnter: KEY.ENTER,
  Escape: KEY.ESCAPE,
  KeyR: KEY.R,
};

// The HUD's lines under the name.
export const HUD_CONTROLS =
  'W/A/S/D or arrows: roll · Space: jump · click: drop a ball\n' +
  'right-drag: orbit · wheel: zoom\n' +
  'Enter: clear the dropped balls · R: reload the scene\n' +
  'touch: swipe to fling the ball · tap to drop';

// The HUD's status line once the scene is on screen.
export const PLAYING_STATUS = 'playing — roll · Space jump · click drops a ball · right-drag orbit';

// ── Cues ──────────────────────────────────────────────────────────────────
//
// Cue ids are defined by the game (GAME_CUE_* in app/src/game/game.h); this
// page decides what each sounds like. Nothing checks these numbers against
// game.h: change them together. Every cue carries f[1] = a value (intensity
// 0..1, roll speed 0..1, or camera yaw in radians) and f[2..4] = a world
// position x, y, z.
const CUE = {
  FLOOR: 1, // a ball landed on the table
  WALL: 2, // a ball hit a rail
  BALL: 3, // two balls hit each other
  ROLL: 4, // the player's ball rolling, ~20 times a second
  CAMERA: 5, // the camera's yaw changed
};

// The names of the cue ids above, for the stats panel's "last cue" line.
// ROLL and CAMERA stream continuously and would bury everything else, so they
// are not shown.
const CUE_NAMES = {
  [CUE.FLOOR]: 'floor',
  [CUE.WALL]: 'wall',
  [CUE.BALL]: 'ball',
};

// Impact sounds started per frame at most. Each one builds a small WebAudio
// graph on this thread, so a pile of balls landing in the same frame must not
// stall it. (The game already spaces its impact cues out; this is a backstop.)
const MAX_IMPACTS_PER_FRAME = 4;

let cameraYaw = 0; // the latest CAMERA cue, applied once the sound has loaded
let rollX = 0; // where the latest ROLL cue put the ball, so the roll can be
let rollZ = 0; // silenced without moving its sound
let impacts = 0; // impact sounds started in frame `impactsFrame`
let impactsFrame = -1;

// Voices one cue. `value` is f[1] of the record and (x, z) its position on the
// table; height does not matter to the sound. `page.audio` is the loaded
// `Audio` (audio/src/lib.rs), or null while the sound is unavailable.
function soundCue(page, cue, value, x, z) {
  const audio = page.audio;
  if (!audio) return;
  try {
    switch (cue) {
      case CUE.FLOOR:
        audio.land(x, z, value);
        break;
      case CUE.WALL:
        audio.wallHit(x, z, value);
        break;
      case CUE.BALL:
        audio.ballClack(x, z, value);
        break;
      case CUE.ROLL:
        audio.roll(value, x, z);
        break;
      case CUE.CAMERA:
        audio.setCameraYaw(value);
        break;
    }
  } catch (err) {
    // After a panic every call into the module throws; the page stops
    // calling it.
    page.soundFailed(err);
  }
}

// Called once per cue the game sent (runtime_send_cue in C), on the frame
// after it. `record.u(0)` is the cue id, `record.f(1)` its value and
// `record.f(2..4)` the world position. `record` is reused for the next cue,
// so copy out what you keep. `page` has:
//   audio            the loaded sound module, or null
//   soundFailed(e)   turn the sound off after `audio` threw
//   showCue(n, v)    the stats panel's "last cue" line: a name and a value
//   frame            counts the page's frames (to limit work per frame)
export function onGameCue(record, page) {
  const cue = record.u(0);
  const value = record.f(1);
  const x = record.f(2);
  const z = record.f(4);
  if (cue === CUE.ROLL) {
    rollX = x;
    rollZ = z;
  } else if (cue === CUE.CAMERA) {
    cameraYaw = value;
  } else {
    page.showCue(CUE_NAMES[cue] ?? `cue ${cue}`, value);
    if (page.frame !== impactsFrame) {
      impactsFrame = page.frame;
      impacts = 0;
    }
    if (impacts >= MAX_IMPACTS_PER_FRAME) return;
    impacts += 1;
  }
  soundCue(page, cue, value, x, z);
}

// The sound has just loaded (it may be after the first cues): bring it up to
// date. A throw here turns the sound off.
export function onSoundLoaded(audio) {
  // The camera may have turned while the sound was loading.
  audio.setCameraYaw(cameraYaw);
}

// The scene is going away: it is being loaded again, or the app worker is
// being replaced. No cue comes until the next scene is up, so stop anything
// continuous: the roll would keep playing at its last speed.
export function onSceneGone(page) {
  soundCue(page, CUE.ROLL, 0, rollX, rollZ);
}

// Rows of your own for the stats panel's app group, under the step's phases.
// `activePoses` is how many pose slots were in use at the last sample.
export function gameStatsRows(activePoses) {
  return [['balls dropped', `${Math.max(activePoses - 1, 0)}`]]; // besides the red one
}
