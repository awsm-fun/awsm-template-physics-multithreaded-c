# AWSM Template
## Spatial audio and multithreaded Box3D physics, with the app in C

## **▶ [Live site](https://awsm.fun/experiments/box3d-multithreaded-c)**
_the same build also deploys to GitHub Pages on every merge to `main`._

----
A copyable **template** from the [**Awsm**](https://awsm.fun) project, and the
C counterpart of the all-Rust
[physics-multithreaded](https://github.com/awsm-fun/awsm-template-physics-multithreaded)
template: the same scene, physics, sound and controls, with the application
written in C.

To start a project from it, clone with the Box3D submodule:

```sh
git clone --recurse-submodules https://github.com/awsm-fun/awsm-template-physics-multithreaded-c
```

It runs on three threads: a plain JavaScript
page, a WebGPU renderer written in Rust, and an application written in C
(which starts more threads of its own for physics). All
application logic and state live in the C code, which is where you work day to
day. The three threads talk through one block of shared memory, the **bridge
block**, laid out by a single C header, `app/src/bridge.h`. Saving a C file
rebuilds the app and swaps it into the running page in a couple of seconds. The
page does not reload and the renderer keeps its loaded scene.

The included demo is a table with a red ball on it, simulated by
[Box3D](https://github.com/erincatto/box3d) (the real C engine, as a git
submodule, with its solver running on several threads):

| Input | Effect |
| --- | --- |
| W A S D or the arrow keys | roll the red ball (relative to the view) |
| Space | jump |
| click on the table | drop a new ball there (up to 200) |
| drag with the right mouse button | orbit the camera |
| mouse wheel | zoom |
| touch: swipe | throw the red ball |
| touch: tap | drop a new ball |
| R | clear the dropped balls and reset the red one |

Sound is an [awsm-audio](https://github.com/awsm-fun/awsm-audio) project
played on the page: a rolling rumble that follows the red ball's speed, and
knocks, thuds and clacks for hits on the rails, the table and other balls,
each as loud as the impact was hard and placed in stereo where it happened. It
starts on the first key or pointer press, as browsers require.

Over the canvas: the controls, whether sound is on and a status line (top
left); a **Stats** panel (bottom-left button) with frames per second, the GPU
and CPU time per frame, the app's CPU time per simulation step, steps per
second, whether the picture keeps in step with the simulation, Box3D's thread
count, the number of dropped balls and the last impact cue; **About** (bottom
centre); and **Settings** (bottom right): the render resolution and the
anti-aliasing (Off, SMAA, 4× MSAA). Touch devices and software GPUs start at
lower defaults; choices are remembered in `localStorage`, and `?reset` in the
URL forgets them.

## Quick start

You need these tools. `task setup` checks for all of them except Task itself,
and prints the install hint shown here for any that is missing.

| Tool | Install hint (macOS, Homebrew) | Version used |
| --- | --- | --- |
| [Task](https://taskfile.dev) | `brew install go-task` | 3.53 |
| CMake and Ninja | `brew install cmake ninja` | |
| Emscripten (`emcc`) | `brew install emscripten`, or [emsdk](https://emscripten.org) | 7.0.0 |
| Node | `brew install node` (18 or newer) | 24 |
| watchexec | `brew install watchexec` | 2.3 |
| Rust (`cargo`, via rustup) | [rustup.rs](https://rustup.rs) | 1.100 stable |
| Rust wasm target | `rustup target add wasm32-unknown-unknown` | |
| wasm-bindgen CLI | `cargo install wasm-bindgen-cli --locked --version 0.2.126` | 0.2.126 |
| wasm-opt (Binaryen) | `brew install binaryen` | 123 |
| clang-format (optional, for `task fmt`) | `brew install clang-format` | |

The wasm-bindgen CLI must be exactly the version of the `wasm-bindgen` crate
pinned in `renderer/Cargo.toml` and `audio/Cargo.toml` (currently `=0.2.126`),
or it refuses their wasm. Git is needed too: Box3D is a submodule. You also need a browser with WebGPU.

No separate LLVM install is needed. One of the renderer's dependencies
(`meshopt`) contains C++ that has to be compiled to wasm, and Apple's clang
cannot do that, so the renderer tasks use the clang that ships with Emscripten
(`em-config LLVM_ROOT`). To use a different compiler, export
`CC_wasm32_unknown_unknown`, `CXX_wasm32_unknown_unknown` and
`AR_wasm32_unknown_unknown` before running `task`.

Then:

```sh
task setup   # check the toolchain, fetch Box3D, configure the C presets (gives clangd its compile databases)
task dev     # build everything, serve at http://127.0.0.1:8000, open a browser, watch for changes
```

The first `task dev` compiles the two Rust modules from scratch: the renderer,
which takes several minutes (about 4.5 minutes on a cold cargo cache) and
produces a wasm file of about 5 MB, and the much smaller audio module. After
that they are only rebuilt when you ask for it, so everyday iteration never
recompiles Rust.

Development has been on macOS with Chrome. The checks and the release build
also run on Ubuntu in CI (`.github/workflows/`), which doubles as a recipe for
setting up Linux; other browsers with WebGPU have not been tried.

## How it fits together

```
 main thread               render worker               app worker
 dom/ (JavaScript)         renderer/ (Rust, WebGPU)    app/ (C23, Emscripten)
 ┌─────────────────────┐   ┌───────────────────────┐   ┌───────────────────────┐
 │ canvas, keyboard,   │   │ awsm-renderer on an   │   │ all state and logic,  │
 │ pointer, HUD, sound │   │ OffscreenCanvas       │   │ fixed 240 Hz steps    │
 └──────────┬──────────┘   └───────────┬───────────┘   └───────────┬───────────┘
            │ typed-array views        │ typed-array views         │ plain C pointers
            ▼                          ▼                           ▼
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ bridge block, inside the app module's shared WebAssembly.Memory             │
 │ (layout: app/src/bridge.h)                                                  │
 │                                                                             │
 │  header     magic, version, step rate, frame_tick,                          │
 │             latest_step, section table                                      │
 │  input      held keys, pointer, canvas size              dom      → app     │
 │  stats      counters for the stats panel                 app, renderer → dom│
 │  snapshots  ring of 32 world states (camera + poses)     app      → renderer│
 │  ring       key / pointer / wheel events                 dom      → app     │
 │  ring       scene description (colliders)                renderer → app     │
 │  ring       bind / spawn / despawn commands              app      → renderer│
 │  ring       cues (sound, UI)                             app      → dom     │
 └─────────────────────────────────────────────────────────────────────────────┘
```

`postMessage` is used only to start things up: the page hands the canvas to
the renderer, the app worker sends the page its memory and the block's address,
and the page forwards both to the renderer (`attach`). After that it carries
only rare messages: a new canvas size (`resize`, also how the resolution
setting is applied), an anti-aliasing change (`anti-alias`) and the
renderer's progress back while it recompiles for one. Nothing per-frame goes
through `postMessage`.

### Directory layout

```
app/                  the C app: a CMake project (CMakeLists.txt, CMakePresets.json)
  src/app.c, app.h    the demo: all game state and logic
  src/physics.c       the glue between the app and Box3D
  vendored/box3d/     Box3D itself, unmodified (git submodule)
  src/bridge.h        the contract between the three threads (start here)
  src/bridge.c        the app's side of the bridge block
  src/camera.c        orbit camera, click-to-ray picking
  src/fixed_step.c    turns wall-clock time into whole 240 Hz steps
  src/math3d.h        small vector / quaternion / matrix library
  src/web_main.c      browser entry point (app_start, app_run)
  src/main.c          native entry point: a headless run with no browser
  src/headless.c      stand-ins for the page and renderer, for native runs and tests
  tests/              native tests (test_app.c, test_bridge.c)
  web/worker.js       the worker script that loads app.js + app.wasm
audio/                a second Rust crate: plays the sound, on the main thread
  src/lib.rs          the small API the page calls (load, start, roll, wallHit, …)
  src/sound.rs        the awsm-audio player: voices, live parameters, listener
renderer/             the Rust crate (cdylib), built with cargo + wasm-bindgen + wasm-opt
  src/bridge.rs       the renderer's mirror of bridge.h
  src/render.rs       start-up, frame loop, attach / detach
  src/scene.rs        loads the scene bundle, reports colliders, indexes drivable nodes
  src/slots.rs        what each pose slot drives (bind / spawn / despawn)
  src/cursor.rs       which simulation time each frame shows
  web/worker.js       the worker script that loads the wasm-bindgen output
  vendored/           Basis Universal transcoder and its worker (decodes the KTX2 textures)
dom/                  the page: index.html, main.js, bridge.js (mirror of bridge.h), style.css,
                      and the panels: settings.js, stats.js, about.js, loading.js, modal.js
media/bundle/         the scene: scene.toml + assets/ (KTX2 textures)
media/audio/          the sounds: an awsm-audio export (project.toml + assets/)
tools/                dev-server.mjs (dev server), coi-serviceworker.js (header shim)
Taskfile.yml          entry point for every workflow; the steps live in taskfile/*.yml
dist/                 build output (git-ignored); deployable as it is
```

### Why it is built this way

**Why the shared block lives in the C module's memory.** A wasm module can
only address its own linear memory. Two separate modules therefore cannot both
have zero-copy access to the same data: one of them owns it, and the other
reaches it through JavaScript typed-array views. The app is where iteration
happens, so it owns the block and uses plain C pointers. The renderer builds
its views once at attach time and, each frame, copies the snapshots it needs
(the two it interpolates between, one bulk `set()` call each) into buffers it
already owns. Nothing is allocated per frame.

**Why two wasm modules instead of linking the C into the Rust.** Keeping the
app a separate Emscripten module means it keeps a real libc, the native build
with AddressSanitizer and UBSan, and pthreads (Box3D's solver threads). It also
means the app worker can be replaced on its own while the renderer keeps
running, which is what makes the hot swap possible. And the renderer stays an
ordinary single-threaded wasm module on stable Rust, without the nightly
features that Rust wasm threads require.

**Why the app is authoritative.** The app owns the whole world, including the
camera. The renderer is a view: it draws whatever the app last published and
holds no application state of its own. Because the app owns the camera, it can
turn a click into a ray through the scene and get the same answer the picture
shows (`camera_ray` in `app/src/camera.c`).

**Why the app worker never receives messages.** The app's loop (`app_run` in
`app/src/web_main.c`) never returns: it sleeps on a shared word until the
renderer presents a frame, runs as many fixed steps as the elapsed time pays
for, and sleeps again. A worker that never returns to its event loop cannot
receive `postMessage`, so everything that goes into the app goes through the
bridge block.

## Day-to-day work

Run `task dev` and leave it running. It serves `dist/` at
http://127.0.0.1:8000 with the headers shared memory needs, and watches the
sources:

| You edit | What happens |
| --- | --- |
| anything under `app/src`, `app/web`, `app/cmake`, or `app/CMakeLists.txt` / `app/CMakePresets.json` | the app is rebuilt (`task app:dev`) and only the app worker is swapped. No page reload; the renderer keeps its scene. About 2.5 seconds from save to the new code running. |
| anything under `dom/` | the files are copied to `dist/` and the page reloads |
| `renderer/` | not watched. Run `task renderer:release` (or the faster, unoptimized `task renderer:debug`) in another terminal; the page reloads when it finishes. |
| `audio/` | not watched. Run `task audio:release`; the page reloads. |
| `media/bundle/`, `media/audio/` | not watched. Run `task media:build`; the page reloads. |

A hot-swapped app starts from a fresh state: `app_init` runs again, the
renderer puts every object back where the scene file has it and describes the
scene to the new app again. `printf` output from C appears in the browser
console prefixed with `[app]`. If a build fails, the page keeps running the last
good build and the compiler errors show in the terminal.

Variables for `task dev`: `RELOAD=page` reloads the whole page on every change
instead of swapping the app; `OPEN=` does not open a browser; `PORT=9000`
changes the port.

`task dev` builds the renderer only if `dist/renderer.stamp` is missing, and
the audio module only if `dist/audio.stamp` is. Note that `task clean`,
`task renderer:clean` and `task audio:clean` delete cargo's `target/`
directories, so the next build compiles from scratch again.

### Tasks

Run `task` (or `task --list`) for this list.

| Task | What it does |
| --- | --- |
| `task setup` | Check the toolchain (with install hints), then configure the C presets for clangd |
| `task dev` | Build, serve at http://127.0.0.1:8000 and rebuild on change |
| `task build` | Release build of everything into a fresh `dist/`, deployable to any static host as it is (`COI=0` leaves out the header shim) |
| `task serve` | Release build, then serve it (no watching) |
| `task test` | Build and run the native app tests (debug, AddressSanitizer + UBSan) |
| `task check` | Everything CI runs: clean configure, build and test of the app (native debug, native release, wasm) and the renderer and audio checks |
| `task fmt` | Format the C and Rust sources |
| `task clean` | Delete all build output (`dist/`, `app/build/`, the Rust `target/` directories) |
| `task app:run` | Native headless run of the app, no browser. `task app:run -- 20` simulates 20 seconds. |
| `task app:test` | Native debug build and the app tests |
| `task app:check` | Clean-slate configure, build and test: native debug, native release, and wasm (tests run under node) |
| `task app:dev` | Fast-linking debug wasm build (runtime assertions) into `dist/app/` |
| `task app:release` | Optimized (`-O3`) wasm build into `dist/app/` |
| `task app:debug` | Native debug build (AddressSanitizer + UBSan) |
| `task app:setup` | Configure the `debug` and `wasm-dev` presets so clangd has both compile databases |
| `task app:fmt` / `task app:clean` | Format the C sources / delete `app/build/` |
| `task renderer:release` | Optimized renderer build into `dist/renderer/` |
| `task renderer:debug` | Faster renderer build (own code unoptimized, dependencies optimized, no wasm-opt) |
| `task renderer:check` | `cargo check`, `clippy` with warnings as errors, and a format check |
| `task renderer:fmt` / `task renderer:clean` | Format the Rust sources / remove `target/` and the built renderer |
| `task audio:release` | Optimized audio module build into `dist/audio/` |
| `task audio:check` | `cargo check`, `clippy` with warnings as errors, and a format check |
| `task audio:fmt` / `task audio:clean` | Format the Rust sources / remove `target/` and the built module |
| `task dom:build` | Copy `dom/` into `dist/` (`COI=1`, which `task build` passes, adds the service-worker shim) |
| `task media:build` | Copy the scene bundle, the audio project and the Basis transcoder into `dist/` |
| `task dev:server` | Serve `dist/` with the COOP/COEP headers and live reload, without building |

All C build configuration lives in `app/CMakeLists.txt` and
`app/CMakePresets.json`; the tasks are thin wrappers, so `cmake --preset …` in
`app/` does the same thing. The presets are `debug`, `release`, `wasm` and
`wasm-dev`.

## Where to put your code

### Game logic: `app/src/app.c`

`app_step()` runs once per fixed step (`APP_STEP_HZ`, 240 per second) and does
four things in order:

1. `read_scene()`: what the renderer says about the loaded scene;
2. `read_user_input()`: key, pointer and wheel events from the page;
3. `simulate()`: move the world. This is marked "Your game logic goes here";
4. `publish_snapshot()`: write the camera and one pose per slot for the
   renderer.

All state lives in `app_state` (`app/src/app.h`). Read held keys with
`bridge_key_held()`, and handle one-off events in `read_user_input()`. To make
something appear, move or disappear on screen, use `send_command()` with
`BRIDGE_CMD_BIND`, `BRIDGE_CMD_SPAWN` or `BRIDGE_CMD_DESPAWN`, then publish a
pose for that slot every step. Set the pose's `BRIDGE_POSE_TELEPORT` flag when
an object jumps (the demo does this through `app_ball.teleported`), so
the renderer does not slide it across the world. Send sounds and UI events to
the page with `send_cue()`.

Express rates per second and multiply by `dt`. Then `APP_STEP_HZ` changes the
step size, not the speed of the game.

### Physics: Box3D

The demo's physics is [Box3D](https://github.com/erincatto/box3d), Erin
Catto's 3D engine, in C. It is the upstream repository, unmodified, as a git
submodule in `app/vendored/box3d`, built by its own CMake project into a
static library that the app links (`add_subdirectory` in `app/CMakeLists.txt`).
Nothing of Box3D is copied into this repository.

* **Getting it.** `task setup` fetches the submodule. By hand:
  `git submodule update --init`, or clone with `--recurse-submodules`.
* **Updating it.** Check out the commit you want inside `app/vendored/box3d`
  and commit the changed submodule pointer. The next build recompiles it.
* **Using it.** `app.c` calls the `b3*` API directly: `scene_ready()` turns
  the colliders the renderer reported into static bodies, `create_ball()`
  makes the dynamic spheres, `simulate()` calls `b3World_Step`, and
  `read_contact_events()` turns Box3D's touch and hit events into sound cues.
  `physics.c` holds the few pieces that are not specific to the demo (world
  creation, the assert hook, collider record → Box3D shape).
* **Speed.** Box3D is always compiled optimized, even in the debug presets
  (its asserts stay on there), and on wasm with SIMD. It is compiled once, so
  it does not slow down the save-to-running loop.
* **Threads.** In the browser the solver runs on the app thread plus up to
  four more, depending on the machine's cores (`solver_thread_count()` in
  `app/src/web_main.c`). These are Emscripten pthreads: web workers that share
  the app's memory. They are created while the module loads
  (`-sPTHREAD_POOL_SIZE` in `app/CMakeLists.txt`), because the app thread
  never returns to its event loop afterwards and could not start one. For the
  same reason the app keeps one Box3D world for its whole life and replaces
  the bodies in it, rather than recreating the world. Native runs and tests
  use one thread.

The app's memory has a fixed size of 64 MB (`-sINITIAL_MEMORY` in
`app/CMakeLists.txt`), because the other threads hold views of it and growing
it would replace the buffer under them. Raise it there if you need more.

Native builds run the same code without a browser. `task app:run` drives it
with a short script of input, and `app/tests/test_app.c` shows how to test game
logic through the bridge, using the stand-ins in `app/src/headless.h`
(`headless_send_scene`, `headless_key`, `headless_click`, `headless_swipe`,
`headless_drain`).

### Sound: cues, `dom/main.js` and `audio/`

The app never plays anything. It sends **cues** to the page (`send_cue()`),
and the page turns them into sound. The demo's cues (`APP_CUE_*` in
`app/src/app.h`):

| Cue | Sent when | Becomes |
| --- | --- | --- |
| `FLOOR`, `WALL`, `BALL` | Box3D reports a hit faster than 0.8 m/s, at most one of each kind per 0.13 s | a one-shot thud, knock or clack, with the hit's strength and position |
| `ROLL` | 20 times a second | the level and tone of the looping rolling sound, from the red ball's speed on the table |
| `CAMERA` | the camera's yaw changed | where the listener sits, so left and right in the sound match the view |

WebAudio only exists on the main thread, so the player is a module the page
imports: `audio/`, a small Rust crate built like the renderer (cargo,
wasm-bindgen, wasm-opt) around
[awsm-audio-player](https://crates.io/crates/awsm-audio-player). It plays the
awsm-audio project in `media/audio/`, which `task media:build` copies to
`dist/audio-project/`. If the module or the project fails to load, the HUD
says `sound: off` and everything else carries on.

To change how things sound, edit the project in the awsm-audio editor and
export it over `media/audio/`. Keep the sample names and node labels
(`roll_LEVEL`, `hit_FILTER`, `land_PANNER`, `clack_LEVEL`, …):
`audio/src/sound.rs` finds the controls it drives by them.

### Example: add a sound cue

Cue ids belong to the app; the page decides what each one sounds like. To make
the jump audible, reusing the landing thud:

`app/src/app.h`, add an id:

```c
    APP_CUE_CAMERA = 5, // the camera turned
    APP_CUE_JUMP = 6,   // the player's ball jumped
```

`app/src/app.c`, send it from `jump()`, with an intensity from 0 to 1 and a
position:

```c
    send_cue(app, APP_CUE_JUMP, 0.5f, player(app)->position);
```

`dom/main.js`, add the id to `CUE` and `CUE_NAMES`, and a case in
`soundCue()`:

```js
    case CUE.JUMP:
      audio.land(x, z, value);
      break;
```

Without that case the cue is silent, but still shows in the stats panel (as
`cue 6` if it has no name). Neither Rust module needs rebuilding. A cue that needs a
sound of its own also needs that sound in the awsm-audio project and a method
for it in `audio/src/lib.rs`, followed by `task audio:release`.

### Example: use a key

The page reports a fixed set of keys (`BRIDGE_KEY_*` in `app/src/bridge.h`);
what a key does is decided in the app. Each key arrives two ways: as a held bit
you can poll every step, and as a `BRIDGE_EV_KEY` event when it goes down or
up.

Shift is already reported but unused. To make it a "boost", use the held state
in `steer_player()`:

```c
float accel = bridge_key_held(input, BRIDGE_KEY_SHIFT) ? 2.0f * MOVE_ACCEL : MOVE_ACCEL;
ball->velocity = vec3_add(ball->velocity, vec3_scale(push, accel * dt));
```

To add a key the page does not report yet, for example F:

1. `app/src/bridge.h`: add `BRIDGE_KEY_F = 14,` after `BRIDGE_KEY_R` (there is
   room up to `BRIDGE_KEY_COUNT`, 64).
2. `dom/bridge.js`: add `F: 14,` to `KEY`.
3. `dom/main.js`: add `KeyF: KEY.F,` to `KEY_CODES` (the keys are
   `KeyboardEvent.code` values, so the physical key is the same on every
   layout).
4. `app/src/app.c`: react to it in `read_user_input()`, next to the
   `BRIDGE_KEY_SPACE` and `BRIDGE_KEY_R` cases, or poll it with
   `bridge_key_held()`.

This is an additive change, so `BRIDGE_VERSION` stays the same and the renderer
is not rebuilt.

### Example: add a record type

Discrete events travel as 64-byte `bridge_record`s: a `type`, the app `step`,
and 14 payload words readable as `u[i]` (unsigned) or `f[i]` (float). The
existing types are grouped by direction (1–5 page to app, 16–18 renderer to
app, 32–34 app to renderer, 48 app to page). Add a new type to the enum in
`app/src/bridge.h`, next to its group, and document its payload there. Every
consumer skips types it does not know.

Whether the renderer needs rebuilding depends only on whether it has to
understand the new record:

| Direction | Producer | Consumer | Renderer rebuild? |
| --- | --- | --- | --- |
| page → app | a new `send…` method on `Bridge` in `dom/bridge.js` (built on its private `#push`) | `read_user_input()` in `app.c` | no |
| app → page | `bridge_ring_push()` on `BRIDGE_RING_APP_TO_DOM` | the `bridge.drain()` callback in `dom/main.js` (add the type to `RECORD` in `dom/bridge.js`) | no |
| app → renderer | `bridge_ring_push()` on `BRIDGE_RING_APP_TO_RENDERER` | `renderer/src/bridge.rs`, `render.rs`, `slots.rs` | yes |
| renderer → app | `renderer/src/bridge.rs` and `render.rs` | `read_scene()` in `app.c` | yes |

A renderer that predates a new app-to-renderer type logs a warning once and
skips those records, so the page keeps working while you rebuild.

For example, an app-to-page record that reports a score (the names here are
new ones you would add):

```c
// app/src/bridge.h, in the record enum, after BRIDGE_CUE:
    // The score changed.          u[0] = new score
    BRIDGE_UI_SCORE = 49,

// app/src/app.c:
bridge_record rec = {.type = BRIDGE_UI_SCORE, .step = app->step};
rec.u[0] = score;
(void)bridge_ring_push(bridge_get_ring(app->bridge, BRIDGE_RING_APP_TO_DOM), &rec);
```

```js
// dom/bridge.js, in RECORD:
  UI_SCORE: 49,

// dom/main.js, at the top of the bridge.drain() callback in frame():
if (record.type === RECORD.UI_SCORE) {
  hudScore = record.u(0);
  return;
}
```

### What needs a renderer rebuild

| Change | Rebuild the renderer? |
| --- | --- |
| anything in `app.c` / `app.h`, including `APP_STEP_HZ` | no |
| new keys, new cue ids, new page↔app record types | no |
| `BRIDGE_SNAPSHOT_COUNT`, `BRIDGE_POSE_CAPACITY`, `BRIDGE_RING_CAPACITY` (powers of two where the header says so) | no; the renderer reads them from the section table |
| new or larger sections, new fields in place of `reserved` words or at the end of a struct | no (rule 1 in `bridge.h`) |
| new app↔renderer record types, new collider shapes | yes |
| anything that changes what existing data means | yes, and bump `BRIDGE_VERSION` |
| anything about drawing | yes |

## The bridge in more depth

`app/src/bridge.h` is the source of truth. It documents every field, every
record payload and the rules below; `renderer/src/bridge.rs` and
`dom/bridge.js` mirror it, and are the only files in their halves of the
project that know byte offsets. This section is the overview.

**Snapshots and pacing.** Every fixed step, the app writes a snapshot (the
camera plus one pose per slot) into the next entry of a ring that keeps the
newest `BRIDGE_SNAPSHOT_COUNT` (32) steps, then stores that step number in
`latest_step`. Once per presented frame the renderer increments `frame_tick`
and wakes the app, which has been waiting on that word
(`bridge_wait_frame()`). The app then runs as many steps as the elapsed time
pays for (`app/src/fixed_step.c`, which caps a single catch-up at 100 ms).
Pacing the app from the display keeps the two in phase; a separate timer would
drift against it.

The app does not wake at perfectly even intervals, so the renderer does not
draw the newest step. It keeps its own clock that trails `latest_step` by a
small lag (at least 2 steps, about 8 ms at 240 Hz, and more if wake-ups get
uneven; see `renderer/src/cursor.rs`), and interpolates between the two
snapshots either side of it. The snapshot ring is the jitter buffer in between.
Commands are stamped with the step they were sent at and take effect when the
display reaches that step, so a spawn never shows up before the frame it
belongs to.

**Event rings.** Four single-producer, single-consumer queues carry fixed-size
tagged records: page → app (input events), renderer → app (the scene
description: `BRIDGE_SCENE_BEGIN`, one `BRIDGE_SCENE_COLLIDER` per collider,
`BRIDGE_SCENE_READY`), app → renderer (`BRIDGE_CMD_BIND`, `BRIDGE_CMD_SPAWN`,
`BRIDGE_CMD_DESPAWN`), and app → page (`BRIDGE_CUE`). Names never cross as
strings: scene nodes and material variants are identified by `bridge_hash()`,
a 32-bit FNV-1a hash that C, Rust and JavaScript compute identically.

**Concurrency rules.**

* Every field marked `_Atomic` in `bridge.h` is accessed atomically by all three
  sides (`Atomics.*` in JavaScript).
* Each snapshot is guarded by a seqlock: its `version` is odd while the app
  writes it. A reader copies the snapshot and keeps the copy only if `version`
  was even and unchanged.
* Each ring has one producer, which owns `head`, and one consumer, which owns
  `tail`. Neither side ever blocks or takes a lock. A full ring drops the record
  and counts it in `dropped`, except that the renderer holds back its scene
  description and retries on the next frame.
* The only waiting anywhere is the app sleeping on `frame_tick`.

**Compatibility.** The header begins with a section table (offset, size,
stride and count of everything after it), and readers take positions from that
table at attach time, never from constants. Layout changes are additive:
add fields, append record types, grow capacities, add sections. None of that
requires rebuilding the renderer, which is why normal C iteration never
touches Rust. A change in meaning bumps `BRIDGE_VERSION` (in `bridge.h`, and
the matching constants `VERSION` in `renderer/src/bridge.rs` and
`BRIDGE_VERSION` in `dom/bridge.js`). On a mismatch the renderer and the page
refuse to attach and say so on the page, instead of drawing garbage. The
`static_assert` layout guards at the end of `bridge.h` catch a field that moved
by accident.

## Deploying

`task build` writes a complete static site to `dist/` (about 25 MB, most of it
scene textures; the release wasm files are about 5 MB for the renderer, 1.1 MB
for audio and 0.5 MB for the app including Box3D). Copy the directory to any static host.

Shared wasm memory needs a SharedArrayBuffer, and browsers only allow that on a
**cross-origin isolated** page, served over HTTPS (or from localhost) with these
response headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

The dev server sends them (`tools/dev-server.mjs`), and your production host
must too. With `require-corp`, any resource from another origin would also need
to opt in; everything in `dist/` is same-origin. Serve `.wasm` files as
`application/wasm`.

Many static hosts cannot set headers (GitHub Pages, a storage bucket behind a
CDN), so `task build` also includes `tools/coi-serviceworker.js`, a small
service worker that adds the headers itself. On the first visit the page
reloads once while it installs; where the host already sends the headers it
does nothing. That is how the live site and the GitHub Pages copy of this
template run. `task build COI=0` leaves the shim out.

All URLs in the page and the workers are relative to the page, so the site
works from a subpath (for example `https://example.com/my-game/`) without any
configuration. If isolation is missing, the page shows an error that explains
the headers rather than failing silently.

## Using your own scene

The scene is `media/bundle/`: a `scene.toml` and an `assets/` directory,
an awsm-scene player export. The renderer loads `bundle/scene.toml` from the
site root, fetches the assets next to it, and draws the scene with
awsm-renderer's scene loader. The app never sees the scene file itself: the
renderer reports its colliders, and the app refers to nodes and material
variants by name.

Replace the bundle and run `task media:build`. No code needs rebuilding as long
as the scene provides what the demo looks for, by name:

| Name in the scene | Used for | If missing |
| --- | --- | --- |
| `Ball` | a top-level node. Its first mesh becomes the player's ball (bound to slot 0), and clicks spawn copies of it. | nothing is drawn for the balls; the renderer logs a warning |
| a sphere collider under `Ball` | ball radius and start position (every other collider becomes fixed geometry) | radius 0.3 m, starting at the centre of the table |
| `Ball_Player_Red` | a material variant on the ball's mesh, applied to the player's ball | the player's ball keeps its normal material |
| `Tabletop_Collider` | a box collider: the table top. Clicks drop balls inside its outline, and landing on it makes the "floor" sound. | the box collider with the largest footprint is used; with no boxes at all, clicks drop inside a 6 × 4 m area at height 0 |
| every other collider | a static Box3D body with that shape (rails, obstacles, ramps); hitting one makes the "wall" sound | |

To match a different scene, change the names at the top of `app/src/app.c`
(`BALL_NODE`, `PLAYER_VARIANT`, `TABLE_COLLIDER`). The native runs and tests
do not load the bundle; `headless_send_scene()` in `app/src/headless.c`
describes the bundled scene's colliders by hand, so update it if you want the
tests to match your scene.

What the renderer can drive and report:

* **Bind and spawn** address a node by name. Naming a group addresses the first
  plain mesh inside it. Skinned meshes and cluster-LOD meshes cannot be driven,
  because posing them takes more than a transform.
* **Colliders** are reported for box, sphere, capsule, cylinder and cone shapes,
  in world space. Ellipsoid and convex-hull colliders are skipped with a
  console warning. Scale is folded into the dimensions; for a round shape under
  non-uniform scale the largest axis is used.
* Prefab subtrees are templates, not part of the world, and are ignored.

## Limitations and next steps

* **Tested on macOS with Chrome.** CI builds and tests on Ubuntu, but the
  install hints are Homebrew commands and other browsers are untried.
* **The renderer handles a subset of scenes.** See the list above: skinned and
  cluster-LOD meshes cannot be bound or spawned, and ellipsoid and convex-hull
  colliders are not reported.
* **App state does not survive a hot swap.** Each swap starts the app fresh.
* **Fixed app memory.** The app's memory cannot grow (see above).

## Licences and credits

This template is licensed under either of the MIT licence (`LICENSE-MIT`) or
the Apache Licence 2.0 (`LICENSE-APACHE`), at your option.

Third-party pieces included in the repository:

| Path | What | Licence |
| --- | --- | --- |
| `renderer/vendored/basis/` | [Basis Universal](https://github.com/BinomialLLC/basis_universal) transcoder, compiled to wasm | Apache-2.0 (`renderer/vendored/basis/LICENSE-basis_universal`) |
| `renderer/vendored/workers/basis-worker.js` | the Basis worker from [awsm-renderer](https://github.com/awsm-fun/awsm-renderer) | MIT OR Apache-2.0 |
| `app/vendored/box3d/` (git submodule) | [Box3D](https://github.com/erincatto/box3d) by Erin Catto | MIT (`app/vendored/box3d/LICENSE`) |
| `tools/coi-serviceworker.js` | coi-serviceworker v0.1.7, by Guido Zuidhof and contributors | MIT |

The renderer is built on [awsm-renderer](https://github.com/awsm-fun/awsm-renderer)
(MIT OR Apache-2.0) and its scene crates, and the sound on
[awsm-audio](https://github.com/awsm-fun/awsm-audio)'s player, all fetched by
cargo. The scene in `media/bundle/` and the sounds in `media/audio/` are the
ones from the awsm physics-multithreaded template.
