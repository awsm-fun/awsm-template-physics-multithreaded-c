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
block**, laid out by a single C header, `app/src/runtime/bridge.h`. Saving a
C file rebuilds the app and swaps it into the running page in a couple of
seconds. The page does not reload and the renderer keeps its loaded scene.

## Where your game goes

Every file is either **yours**, the part that changes when you make a new
game from this template, or **the runtime**, which is the same for every game
and runs yours. The C and JS files under `game/` and `runtime/` say which on
their first line (`YOUR GAME: start here` or `RUNTIME: …`).

```
app/src/game/        YOURS    game.h, game.c: your game, in C. Start here.
                              headless.c: your scene and input for native runs
app/tests/game/      YOURS    test_game.c: tests for your game
dom/game.js          YOURS    page side: name, storage prefix, keys, HUD, cues → sound
dom/index.html       YOURS    page shell: title and About text
media/bundle/        YOURS    the scene: scene.toml (+ assets/)
media/audio/         YOURS    the sounds: an awsm-audio project

app/src/runtime/     RUNTIME  step loop, Box3D world and pose slots, camera, bridge block, entry points
app/tests/runtime/   RUNTIME  bridge, layout, fixed-step and camera tests, check.h
app/web/             RUNTIME  the app worker's script
app/vendored/box3d/  RUNTIME  Box3D itself (git submodule)
dom/runtime/         RUNTIME  boot, workers, input, HUD, sound loading, panels, bridge view
renderer/            RUNTIME  the Rust WebGPU renderer
audio/               RUNTIME  the Rust sound player your cues call
tools/, taskfile/    RUNTIME  dev server, checks, every workflow
```

To replace the ball demo with your own physics game, edit `app/src/game/`,
`dom/game.js` and `media/` (and retitle `dom/index.html`); nothing in
`renderer/`, `audio/`, `tools/` or `taskfile/` needs to change. The runtime
calls your game through a few hooks in `game.c`, and your game acts only
through `runtime_*` calls and Box3D's own `b3*` API on its bodies; it never
touches the bridge block itself (see [Your game](#your-game)). A new `.c` file in
`app/src/game/` or `test_*.c` in `app/tests/game/` is picked up by the build
with no other edit. The only reason to open a runtime file is something the
runtime does not have yet, such as a new key, a new kind of message between
the threads, or a sound the player cannot make yet; the steps below say when.

## The demo

A table with a red ball on it, simulated by
[Box3D](https://github.com/erincatto/box3d) (the real C library, as a git
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
| Enter | clear the dropped balls and reset the red one |
| R | reload the whole scene (same page, same GPU device) |

Sound is an [awsm-audio](https://github.com/awsm-fun/awsm-audio) project
played on the page: a rolling rumble that follows the red ball's speed, and
knocks, thuds and clacks for hits on the rails, the table and other balls,
each as loud as the impact was hard and placed in stereo where it happened. It
starts on the first key or pointer press, as browsers require.

Over the canvas: the controls, whether sound is on and a status line (top
left); a **Stats** panel (bottom-left button, described below); **About** (bottom
centre), which says what the demo is and, on a second tab, explains every
number in the Stats panel; and **Settings** (bottom right): the render resolution and the
anti-aliasing (Off, SMAA, 4× MSAA). The resolution starts at native, held to
a budget of 2.8 million pixels (so a big high-DPI window starts below 100%);
touch devices and software GPUs start lower. Choices are remembered in
`localStorage` and always win; `?reset` in the URL forgets them. The storage
keys share one prefix (`STORAGE_PREFIX` in `dom/game.js`): change it for
your own project, since everything on the same origin shares `localStorage`.

The Stats panel opens with the frame budget and then has one group per
thread, so each number sits under the thread that spent it, with its unit and
what it is per:

| Group | Headline | Expanded |
| --- | --- | --- |
| frame budget | the time one display refresh allows (16.7 ms at 60 Hz) and the headroom left by whichever of the GPU and the threads uses most of it | the GPU and each thread: time per frame, out of the budget (the app worker's is its whole step, Box3D's included, times the steps per frame); and, for reference, the latency of a frame (renderer + GPU) |
| display | refresh rate and frames per second | missed frames (refreshes with no new frame: any steady count is visible stutter) |
| GPU | GPU time per frame | per render pass (on Apple GPUs the passes overlap, so they add up to more than the frame) |
| renderer (render worker) | the render worker's CPU time per frame | its phases (reading the app's state, camera, transforms, `render()`) and, under `render()`, what awsm-renderer spends preparing the frame (uploads, bind groups, …), encoding each pass and submitting, with what is left over |
| app (C, app worker) | the app's own CPU time per step, without Box3D's | steps per second, frames the app was late for (its steps did not finish within a frame), the phases of a step, dropped balls, the last impact cue |
| physics (Box3D, N threads) | the time of Box3D's step, which the app worker runs together with the solver threads | Box3D's stages (pairs, collide, solve, sensors) and the parts of its solver |
| page (main thread) | the main thread's work per frame | |

A header toggles its group, and the choice is remembered. The breakdowns are
only measured while their group is open (`renderer/src/profile.rs`, "Timing"
in `app/src/runtime/runtime.c`), so a collapsed panel costs nothing extra.

CPU and GPU work run in parallel, so each must fit the frame budget on its
own; they do not add up, except when the CPU waits on the GPU, which shows as
missed frames.

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
which takes several minutes on a cold cargo cache, and the much smaller audio
module. After that each is rebuilt incrementally, in seconds, and only when
one of its own files changes: working on the C app never runs cargo.

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
 │ (layout: app/src/runtime/bridge.h)                                          │
 │                                                                             │
 │  header     magic, version, step rate, frame_tick, frame time,              │
 │             latest_step, section table                                      │
 │  input      held keys, pointer, canvas size              dom      → app     │
 │  stats      counters for the stats panel                 app, renderer → dom│
 │  timings    named totals for the panel's breakdowns      app, renderer → dom│
 │  snapshots  ring of 32 world states (camera + poses)     app      → renderer│
 │  ring       key / pointer / wheel events, frame marks    dom      → app     │
 │  ring       scene description (colliders)                renderer → app     │
 │  ring       bind / spawn / despawn / reload commands     app      → renderer│
 │  ring       cues (sound, UI)                             app      → dom     │
 └─────────────────────────────────────────────────────────────────────────────┘
```

`postMessage` is used only to start things up: the page hands the canvas to
the renderer, the app worker sends the page its memory and the block's address,
and the page forwards both to the renderer (`attach`). After that it carries
only rare messages: a new canvas size (`resize`, also how the resolution
setting is applied), an anti-aliasing change (`anti-alias`), and the
renderer's progress back while it recompiles for one or loads the scene again
(`loading`, `progress`, `ready`). Nothing per-frame goes through
`postMessage`.

### Directory layout

```
app/                  the C app: a CMake project (CMakeLists.txt, CMakePresets.json)
  src/game/           YOUR GAME
    game.h            slots, cue ids, tuning, world settings, game_state, step rate
    game.c            the demo: bodies, controls, drops, cues, camera behaviour
    headless.c        the scene and a script of input, for native runs and tests
  src/runtime/        RUNTIME
    runtime.h, .c     the step loop, the hooks it calls and the runtime_* API
    bridge.h          the contract between the three threads (start here)
    bridge.c          the app's side of the bridge block
    physics.c         Box3D world creation, its assert hook, collider → shape
    camera.c          orbit camera, click-to-ray picking
    fixed_step.c      turns wall-clock time into whole fixed steps
    math3d.h          small vector / quaternion / matrix library
    web_main.c        browser entry point (app_start, app_run)
    main.c            native entry point: a headless run with no browser
    headless.c        stand-ins for the page and renderer, for native runs and tests
  tests/game/         YOUR GAME: test_game.c
  tests/runtime/      RUNTIME: test_bridge.c, test_fixed_step.c, test_camera.c,
                      check.h, and bridge_layout.c (prints the layout for the mirror check)
  vendored/box3d/     Box3D itself, unmodified (git submodule)
  web/worker.js       the worker script that loads app.js + app.wasm
audio/                a second Rust crate: plays the sound, on the main thread
  src/lib.rs          the small API the page calls (load, start, roll, wallHit, …)
  src/sound.rs        the awsm-audio player: voices, live parameters, listener
renderer/             the Rust crate (cdylib), built with cargo + wasm-bindgen + wasm-opt
  src/bridge.rs       the renderer's mirror of bridge.h
  src/render.rs       start-up, frame loop, attach / detach
  src/scene.rs        loads the scene bundle, reports colliders, indexes drivable nodes
  src/slots.rs        what each pose slot drives (bind / spawn / despawn)
  web/worker.js       the worker script that loads the wasm-bindgen output
  vendored/           Basis Universal transcoder and its worker (decodes the KTX2 textures)
dom/                  the page
  index.html          the page shell (YOURS: title and About text)
  game.js             YOUR GAME: name, keys, HUD words, what each cue sounds like
  runtime/            RUNTIME: main.js, bridge.js (mirror of bridge.h), style.css,
                      and the panels: settings.js, stats.js, about.js, loading.js, modal.js
media/bundle/         the scene: scene.toml + assets/ (KTX2 textures)
media/audio/          the sounds: an awsm-audio export (project.toml + assets/)
tools/                dev-server.mjs (dev server), check-bridge.mjs (mirror check),
                      coi-serviceworker.js (header shim)
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
shows (`camera_ray` in `app/src/runtime/camera.c`).

**Why the app worker never receives messages.** The app's loop (`app_run` in
`app/src/runtime/web_main.c`) never returns: it sleeps on a shared word until the
renderer starts a frame, runs as many fixed steps as the time since the
previous frame pays for, and sleeps again. A worker that never returns to its event loop cannot
receive `postMessage`, so everything that goes into the app goes through the
bridge block.

## Day-to-day work

Run `task dev` and leave it running. It serves `dist/` at
http://127.0.0.1:8000 with the headers shared memory needs, and watches the
sources. Each watcher rebuilds only its own part (the watched paths are
disjoint and contain no build output), so a C save never runs cargo:

| You edit | What happens |
| --- | --- |
| `app/src` (your `app/src/game/` included), `app/web`, `app/cmake`, `app/CMakeLists.txt` | the app is rebuilt (`task app:dev`) and only the app worker is swapped. No page reload. |
| `renderer/src`, `renderer/web`, `renderer/Cargo.toml` | `task renderer:dev` (this crate unoptimized, dependencies optimized, no wasm-opt: a few seconds), then the page reloads |
| `audio/src`, `audio/Cargo.toml` | `task audio:dev`, then the page reloads |
| `dom/` (your `dom/game.js` included) | the files are copied to `dist/` and the page reloads |
| `media/`, `renderer/vendored/` | `task media:build` (a copy), then the page reloads |

A hot-swapped app starts from a fresh state: `runtime_init` (and with it
`game_init`) runs again and the renderer describes the scene to the new app.
With `GAME_RELOAD_SCENE_ON_HOT_SWAP` at `1` (the default, in
`app/src/game/game.h`) the
new app also asks the renderer to load the scene again, so every save starts
from the bundle whatever the previous build did to the world; the loading
screen shows while that takes. At `0` it takes over the renderer's current
world (every bind and spawn of the previous app undone), which is instant.
Either way the page and the WebGPU device stay.

`printf` output from C appears in the browser console prefixed with `[app]`.
If a build fails, the page keeps running the last good build, the compiler
errors show in the terminal, and the other watchers keep going. After changing
`app/CMakePresets.json`, run `task app:clean` (a preset is only read when a
build tree is first configured).

Variables for `task dev`: `RELOAD=page` reloads the whole page on every change
instead of swapping the app; `OPEN=` does not open a browser; `PORT=9000`
changes the port.

`task dev` builds the dev profile of both Rust modules; `task build` builds
the release profile (LTO, wasm-opt), which cargo keeps separately. Note that
`task clean`, `task renderer:clean` and `task audio:clean` delete cargo's
`target/` directories, so the next build compiles from scratch again.

### Tasks

Run `task` (or `task --list`) for this list.

| Task | What it does |
| --- | --- |
| `task setup` | Check the toolchain (with install hints), then configure the C presets for clangd |
| `task dev` | Build, serve at http://127.0.0.1:8000, and on every save rebuild just the part that changed |
| `task build` | Release build of everything into a fresh `dist/`, deployable to any static host as it is (`COI=0` leaves out the header shim) |
| `task serve` | Release build, then serve it (no watching) |
| `task test` | Build and run the native app tests (debug, AddressSanitizer + UBSan), including the check that the Rust and JS mirrors of `bridge.h` match it |
| `task check` | Everything CI runs: clean configure, build and test of the app (native debug, native release, wasm), the renderer and audio checks, and a syntax check of the page's JavaScript |
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
| `task renderer:dev` | Incremental renderer build into `dist/renderer/` (own code unoptimized, dependencies optimized, no wasm-opt) |
| `task renderer:release` | Optimized renderer build into `dist/renderer/` |
| `task renderer:check` | `cargo check`, `clippy` with warnings as errors, and a format check |
| `task renderer:fmt` / `task renderer:clean` | Format the Rust sources / remove `target/` and the built renderer |
| `task audio:dev` | Incremental audio module build into `dist/audio/` |
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

## Your game

Your C is `app/src/game/`: `game.h` holds what the runtime needs to know at
compile time (the pose slots `GAME_*_SLOT` and `GAME_SLOT_COUNT`, the cue ids
`GAME_CUE_*`, your `game_state`, tuning, the world settings `GAME_GRAVITY`,
`GAME_HIT_MIN_SPEED` and `GAME_SOLVER_SUB_STEPS`, `APP_STEP_HZ` and
`GAME_RELOAD_SCENE_ON_HOT_SWAP`), and `game.c` the logic. `headless.c` there
describes your scene and scripts some input for the native runs and tests.

Each fixed step (`APP_STEP_HZ`, 240 per second) the runtime
(`app/src/runtime/runtime.c`) reads the scene description and the input
(in the browser, one page frame's worth, in the first step of each frame's
burst; see "Frame pacing" below), lets your game act, steps the Box3D world,
copies the poses out, and publishes a snapshot. The contract is declared in
`app/src/runtime/runtime.h`. `game.c` provides:

| Hook | Called |
| --- | --- |
| `game_init(runtime, game)` | once, when the app starts (after every hot swap too): set up the camera |
| `game_scene_begin(runtime, game)` | a scene description begins (first load, every reload, a hot swap): the runtime has destroyed every body; reset your state |
| `game_scene_ready(runtime, game)` | the scene's colliders are in: build your world from them |
| `game_key(runtime, game, key, down)` | a key went down or up, while the scene is ready: one-off actions |
| `game_pointer(runtime, game, pointer)` | a pointer press, move, release or wheel turn, whether or not the scene is ready |
| `game_update(runtime, game, dt)` | every fixed step while the scene is ready, before the world steps: your game loop |
| `game_contact(runtime, game, a, b, touching)` | after the world step: two shapes started or stopped touching (shapes with `enableContactEvents`) |
| `game_hit(runtime, game, a, b, speed, where)` | after the world step: two shapes hit faster than `GAME_HIT_MIN_SPEED` (shapes with `enableHitEvents`) |
| `game_after_physics(runtime, game)` | the end of the step: every pose is up to date; read results, send cues |

and calls back only these, plus Box3D's own `b3*` API on its bodies:

| Call | Does |
| --- | --- |
| `runtime_collider_count(runtime)`, `runtime_get_collider(runtime, i)` | the scene's colliders: name and top-level hashes, shape, dimensions, pose |
| `runtime_scene_ready(runtime)` | whether the scene is described and the game is running |
| `runtime_reload_scene(runtime)` | the renderer destroys everything and loads the bundle again, keeping the GPU device and the page (R in the demo); every body goes at once and the game restarts from `game_scene_begin` |
| `runtime_current_step(runtime)` | the step being run, counting from 1 |
| `runtime_key_held(runtime, BRIDGE_KEY_W)` | whether a key counts as held this step (a tap shorter than a step counts once) |
| `runtime_pointer_ray(runtime, ndc_x, ndc_y)` | the ray under a point on the canvas, as the picture shows it |
| `runtime_get_camera(runtime)` | the orbit camera the renderer draws from (`app/src/runtime/camera.h`); change it at any time |
| `runtime_world(runtime)` | the Box3D world, to create bodies in |
| `runtime_create_collider_body(runtime, collider, &shape_def)` | a static body for a scene collider, destroyed with the scene |
| `runtime_bind_body(runtime, slot, body, node, variant)` | slot `slot` follows `body` and is drawn as scene node `node` itself, with a material variant or `nullptr` |
| `runtime_spawn_body(runtime, slot, body, node, variant)` | the same, drawn as a new copy of `node` |
| `runtime_destroy_body(runtime, slot)` | destroys the slot's body and takes it off screen |
| `runtime_get_body(runtime, slot)` | the slot's body and its pose as of the last world step |
| `runtime_teleport_body(runtime, slot, position, rotation)` | moves a body at once, at rest, without the renderer gliding it there |
| `runtime_active_bodies(runtime)` | how many slots have a body |
| `runtime_send_cue(runtime, GAME_CUE_ROLL, value, where)` | tells the page something happened (a sound, a UI change) |
| `runtime_send_cue_throttled(runtime, cue, value, where, min_gap_s)` | the same, unless that cue went out less than `min_gap_s` ago |

**Bodies and poses.** Create bodies in `runtime_world()` with `b3CreateBody`
and the shape functions (the material, damping and events are your choice),
then hand each one to be drawn to `runtime_bind_body()` or
`runtime_spawn_body()`. After every world step the runtime copies each slot's
pose out of Box3D and writes it into the snapshot; a new or teleported body is
marked so the renderer does not slide it across the world. The game never
sees a ring or a snapshot, and the runtime never needs to know what a body
means. Express rates per second and multiply by `dt`; then `APP_STEP_HZ`
changes the step size, not the speed of the game.

**The page side** is `dom/game.js`: `GAME_NAME`, `STORAGE_PREFIX`,
`KEY_CODES` (which keys reach the game), `HUD_CONTROLS`, `PLAYING_STATUS`,
`onGameCue(record, page)` (called for every `runtime_send_cue`, with the id
in `record.u(0)`, the value in `record.f(1)` and the position in
`record.f(2..4)`), `onSoundLoaded(audio)`, `onSceneGone(page)` and
`gameStatsRows(activePoses)`. Its `CUE` table must match `GAME_CUE_*` in
`game.h`.

### Physics: Box3D

The demo's physics is [Box3D](https://github.com/erincatto/box3d), Erin
Catto's 3D physics library, in C. It is the upstream repository, unmodified,
as a git submodule in `app/vendored/box3d`, built by its own CMake project
into a static library that the app links (`add_subdirectory` in
`app/CMakeLists.txt`). Nothing of Box3D is copied into this repository.

* **Getting it.** `task setup` fetches the submodule. By hand:
  `git submodule update --init`, or clone with `--recurse-submodules`.
* **Updating it.** Check out the commit you want inside `app/vendored/box3d`
  and commit the changed submodule pointer. The next build recompiles it.
* **Who does what.** The runtime owns the world: it creates it
  (`app/src/runtime/physics.c`, which also routes Box3D's asserts and logs to
  the console and turns a collider record into a Box3D shape), steps it,
  copies the poses out and hands the game Box3D's touch and hit events. Your
  game calls the `b3*` API directly for everything that is its decision:
  `game_scene_ready()` turns the colliders into static bodies with the table's
  materials, `create_ball()` makes the dynamic spheres, `steer_player()` and
  `jump()` push the player's ball, and `game_contact()` / `game_hit()` turn
  the events into sound cues.
* **Speed.** Box3D is always compiled optimized, even in the debug presets
  (its asserts stay on there), and on wasm with SIMD. It is compiled once, so
  it does not slow down the save-to-running loop.
* **Threads.** In the browser the solver runs on the app thread plus up to
  four more, depending on the machine's cores (`solver_thread_count()` in
  `app/src/runtime/web_main.c`). These are Emscripten pthreads: web workers
  that share the app's memory. They are created while the module loads
  (`-sPTHREAD_POOL_SIZE` in `app/CMakeLists.txt`), because the app thread
  never returns to its event loop afterwards and could not start one. For the
  same reason the runtime keeps one Box3D world for the app's whole life and
  replaces the bodies in it, rather than recreating the world. Native runs and
  tests use one thread.

The app's memory has a fixed size of 64 MB (`-sINITIAL_MEMORY` in
`app/CMakeLists.txt`), because the other threads hold views of it and growing
it would replace the buffer under them. Raise it there if you need more.

Native builds run the same code without a browser. `task app:run` drives it
with the script of input in `game_headless_input()`
(`app/src/game/headless.c`), and `app/tests/game/test_game.c` shows how to
test game logic through the bridge, using the stand-ins in
`app/src/runtime/headless.h` (`headless_key`, `headless_click`,
`headless_swipe`, `headless_drain`, …) and your `game_headless_scene()`.

### Sound: cues, `dom/game.js` and `audio/`

The app never plays anything. Your game sends **cues** to the page
(`runtime_send_cue()`), and `onGameCue()` in `dom/game.js` turns them into
sound. The demo's cues (`GAME_CUE_*` in `app/src/game/game.h`):

| Cue | Sent when | Becomes |
| --- | --- | --- |
| `FLOOR`, `WALL`, `BALL` | Box3D reports a hit faster than 0.8 m/s, at most one of each kind per 0.13 s | a one-shot thud, knock or clack, with the hit's strength and position |
| `ROLL` | 20 times a second | the level and tone of the looping rolling sound, from the red ball's speed on the table |
| `CAMERA` | the camera's yaw changed | where the listener sits, so left and right in the sound match the view |

WebAudio only exists on the main thread, so the player is a module the page
imports: `audio/`, a small Rust crate built like the renderer (cargo,
wasm-bindgen, wasm-opt) around
[awsm-audio-player](https://crates.io/crates/awsm-audio-player). The runtime
(`dom/runtime/main.js`) loads it and starts it on the first key or pointer
press; your `dom/game.js` calls its voices. It plays the awsm-audio project in
`media/audio/`, which `task media:build` copies to `dist/audio-project/`. If
the module or the project fails to load, the HUD says `sound: off` and
everything else carries on.

To change how things sound, edit the project in the awsm-audio editor and
export it over `media/audio/`. Keep the sample names and node labels
(`roll_LEVEL`, `hit_FILTER`, `land_PANNER`, `clack_LEVEL`, …):
`audio/src/sound.rs` finds the controls it drives by them.

### Example: add a body

A second ball, "Buddy", that starts beside the player's and stays when Enter
clears the dropped ones. Only your files change.

`app/src/game/game.h`, give it a slot and move the dropped balls up one:

```c
    GAME_PLAYER_SLOT = 0,
    GAME_BUDDY_SLOT = 1,
    GAME_FIRST_DROP_SLOT = 2,
```

`app/src/game/game.c`, at the end of `game_scene_ready()`, before the
`printf`: create its body and draw it as a copy of the ball, in its original
material:

```c
    vec3 buddy = vec3_add(s->ball_start, vec3_make(1.5f, DROP_HEIGHT, 0.0f));
    runtime_spawn_body(runtime, GAME_BUDDY_SLOT,
                       create_ball(runtime, game, GAME_BUDDY_SLOT, buddy), BALL_NODE, nullptr);
```

It drops in, rolls, clacks against the other balls and is put back if it falls
off the table, like any other ball: those rules already cover every slot. No
Rust is rebuilt. (The tests count what the game sends: with Buddy,
`app/tests/game/test_game.c` sees one more SPAWN, one more active body and
one more pose in every snapshot, so update the counts there.)

### Example: add a sound cue

Cue ids belong to the game; `dom/game.js` decides what each one sounds like.
To make the jump audible, reusing the landing thud:

`app/src/game/game.h`, add an id:

```c
    GAME_CUE_CAMERA = 5, // the camera turned
    GAME_CUE_JUMP = 6,   // the player's ball jumped
```

`app/src/game/game.c`, send it from `jump()`, with an intensity from 0 to 1
and a position:

```c
    runtime_send_cue(runtime, GAME_CUE_JUMP, 0.5f,
                     runtime_get_body(runtime, GAME_PLAYER_SLOT)->position);
```

`dom/game.js`, add `JUMP: 6` to `CUE` and `[CUE.JUMP]: 'jump'` to
`CUE_NAMES`, and a case in `soundCue()`:

```js
      case CUE.JUMP:
        audio.land(x, z, value);
        break;
```

Without that case the cue is silent, but still shows in the stats panel (as
`cue 6` if it has no name). Neither Rust module needs rebuilding. A cue that
needs a sound of its own also needs that sound in the awsm-audio project and a
method for it in `audio/src/lib.rs` (a runtime file: the player only has the
demo's voices so far), followed by `task audio:release`.

### Example: use a key

The page reports a fixed set of keys (`BRIDGE_KEY_*` in
`app/src/runtime/bridge.h`); what a key does is decided in the game. Each key
arrives as a `game_key()` call when it goes down or up, and
`runtime_key_held()` answers whether it is held in the current step, worked
out from those events in order. A tap shorter than a frame still counts as
held for one step.

Shift is already reported but unused. To make it a "boost", use the held state
in `steer_player()` in `app/src/game/game.c`:

```c
    float accel = runtime_key_held(runtime, BRIDGE_KEY_SHIFT) ? 2.0f * MOVE_ACCEL : MOVE_ACCEL;
    vec3 push = vec3_scale(vec3_normalize(view_to_ground(runtime, x, z)),
                           accel * dt * game->ball_mass);
```

To add a key the page does not report yet, for example F, three runtime files
change (the key set is part of the bridge) and then yours:

1. runtime: `app/src/runtime/bridge.h`: add `BRIDGE_KEY_F = 14,` after
   `BRIDGE_KEY_R` (there is room up to `BRIDGE_KEY_COUNT`, 64);
   `dom/runtime/bridge.js`: add `F: 14,` to `LAYOUT.key`; and
   `app/tests/runtime/bridge_layout.c`: add `entry("f", BRIDGE_KEY_F);` (the
   mirror check only accepts names that file prints).
2. yours: `dom/game.js`: add `KeyF: KEY.F,` to `KEY_CODES` (the keys are
   `KeyboardEvent.code` values, so the physical key is the same on every
   layout).
3. yours: `app/src/game/game.c`: react to it in `game_key()`, next to the
   `BRIDGE_KEY_SPACE` and `BRIDGE_KEY_R` cases, or poll it with
   `runtime_key_held()`.

This is an additive change, so `BRIDGE_VERSION` stays the same and the renderer
is not rebuilt.

### Example: add a record type

This changes how the threads talk, so it is runtime work, not game work. To
send the page something new without it, use a new `GAME_CUE_*` id instead
(above): that stays in `game.h`, `game.c` and `dom/game.js`.

Discrete events travel as 64-byte `bridge_record`s: a `type`, the app `step`,
and 14 payload words readable as `u[i]` (unsigned) or `f[i]` (float). The
existing types are grouped by direction (1–6 page to app, 16–18 renderer to
app, 32–35 app to renderer, 48 app to page). Add a new type to the enum in
`app/src/runtime/bridge.h`, next to its group, and document its payload there;
add it to `app/tests/runtime/bridge_layout.c` and to the mirror that uses it.
Every consumer skips types it does not know.

Whether the renderer needs rebuilding depends only on whether it has to
understand the new record:

| Direction | Producer | Consumer | Renderer rebuild? |
| --- | --- | --- | --- |
| page → app | a new `send…` method on `Bridge` in `dom/runtime/bridge.js` (built on its private `#push`) | `apply_input()` in `app/src/runtime/runtime.c`, then a hook or `runtime_*` call for your game | no |
| app → page | a new `runtime_*` function in `runtime.c` (`bridge_ring_push()` on `BRIDGE_RING_APP_TO_DOM`) | the `bridge.drain()` callback in `dom/runtime/main.js` (add the type to `LAYOUT.record_type` in `dom/runtime/bridge.js`) | no |
| app → renderer | `bridge_ring_push()` on `BRIDGE_RING_APP_TO_RENDERER` in `runtime.c` | `renderer/src/bridge.rs`, `render.rs`, `slots.rs` | yes |
| renderer → app | `renderer/src/bridge.rs` and `render.rs` | `read_scene()` in `runtime.c` | yes |

A renderer that predates a new app-to-renderer type logs a warning once and
skips those records, so the page keeps working while you rebuild.

For example, an app-to-page record that reports a score (the names here are
new ones you would add):

```c
// app/src/runtime/bridge.h, in the record enum, after BRIDGE_CUE:
    // The score changed.          u[0] = new score
    BRIDGE_UI_SCORE = 49,

// app/src/runtime/runtime.h, with the other cue functions:
void runtime_send_score(runtime_state *runtime, uint32_t score);

// app/src/runtime/runtime.c:
void runtime_send_score(runtime_state *runtime, uint32_t score) {
    bridge_record rec = {.type = BRIDGE_UI_SCORE, .step = runtime->step};
    rec.u[0] = score;
    (void)bridge_ring_push(bridge_get_ring(runtime->bridge, BRIDGE_RING_APP_TO_DOM), &rec);
}

// app/src/game/game.c, wherever the score changes, e.g. at the end of
// drop_ball_at(): one point per ball in play.
    runtime_send_score(runtime, (uint32_t)runtime_active_bodies(runtime) - 1u);
```

```js
// dom/runtime/bridge.js, in LAYOUT.record_type (and
// entry("ui_score", BRIDGE_UI_SCORE) in app/tests/runtime/bridge_layout.c):
    UI_SCORE: 49,

// dom/runtime/main.js, at the top of the bridge.drain() callback in frame():
        if (record.type === RECORD.UI_SCORE) {
          setStatus(`score: ${record.u(0)}`);
          return;
        }
```

### What needs a renderer rebuild

| Change | Rebuild the renderer? |
| --- | --- |
| anything in `app/src/game/`, including `APP_STEP_HZ` | no |
| new keys, new cue ids, new page↔app record types | no |
| `BRIDGE_SNAPSHOT_COUNT`, `BRIDGE_POSE_CAPACITY`, `BRIDGE_RING_CAPACITY` (powers of two where the header says so) | no; the renderer reads them from the section table |
| new or larger sections, new fields in place of `reserved` words or at the end of a struct | no (rule 1 in `bridge.h`) |
| new app↔renderer record types, new collider shapes | yes |
| anything that changes what existing data means | yes, and bump `BRIDGE_VERSION` |
| anything about drawing | yes |

## The bridge in more depth

`app/src/runtime/bridge.h` is the source of truth. It documents every field,
every record payload and the rules below; `renderer/src/bridge.rs` and
`dom/runtime/bridge.js` mirror it, and are the only files in their halves of
the project that know byte offsets. The mirrors cannot drift silently:
`app/tests/runtime/bridge_layout.c` prints the layout as the C compiler sees
it, and
`tools/check-bridge.mjs` compares both mirrors with it (the `bridge_mirrors`
test of `task test`). This section is the overview.

**Frame pacing.** This is Glenn Fiedler's
[Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/) (fixed
steps, rendering interpolated between the last two), split across two threads.
One clock times everything: the browser's frame timestamp
(`performance.timeOrigin` plus the `requestAnimationFrame` time, in
microseconds), which all three threads compute the same way.

1. At the start of every frame the renderer stores that frame's time in the
   header (`frame_time_us`), increments `frame_tick` and wakes the app, which
   has been waiting on that word (`bridge_wait_frame()`).
2. The app runs as many fixed steps as the time since the previous frame time
   pays for (`app/src/runtime/fixed_step.c`, which caps a single catch-up at
   100 ms),
   writing a snapshot per step (the camera plus one pose per slot) into a ring
   that keeps the newest `BRIDGE_SNAPSHOT_COUNT` (32) steps. Each snapshot
   carries the moment its step stands for: the newest one of a burst stands
   for `frame time - leftover`, where the leftover is the part of a step the
   time did not pay for yet.
3. The renderer's NEXT frame draws the moment one step before that frame
   time, interpolating between the two snapshots around it. That moment
   advances by exactly the time between frames, so motion is even however the
   steps fell into bursts (4, 4, 4 at 60 Hz; 1 or 2 at 144 Hz). If the app has
   not published that far yet, the renderer holds the newest step for a frame
   and counts a late frame for the app in the stats panel.

Because the step count comes from the frame times, not from when the app's
thread happens to wake, it does not wobble. Pacing the app from the display
keeps the two in phase; a separate timer would drift against it.

Input goes the same way, one page frame at a time. The page sends its input
records as they happen and, in its own animation frame (if that frame had
any input), a `BRIDGE_EV_FRAME_MARK` with its frame time. The app applies, in the first
step of a burst, the records up to the newest mark that is at least half a
frame older than the burst's frame time, and keeps the rest for the next
burst. So every frame drawn carries exactly one page frame of input, and the
camera, which the app turns from those records, moves evenly.

Latency, from input to the picture: the input waits for the next burst (up to
one frame), which is drawn in the frame after (one more), plus up to a step of
interpolation. About two frames: 33 ms at 60 Hz, 17 ms at 120 Hz. The
pipelining is what lets the app and the renderer run at the same time instead
of taking turns. A dropped frame shows as a pause and then a double step,
rather than a single gap.

Commands are stamped with the step they were sent at and take effect when the
display reaches that step, so a spawn never shows up before the frame it
belongs to.

**Event rings.** Four single-producer, single-consumer queues carry fixed-size
tagged records: page → app (input events), renderer → app (the scene
description: `BRIDGE_SCENE_BEGIN`, one `BRIDGE_SCENE_COLLIDER` per collider,
`BRIDGE_SCENE_READY`), app → renderer (`BRIDGE_CMD_BIND`, `BRIDGE_CMD_SPAWN`,
`BRIDGE_CMD_DESPAWN`, `BRIDGE_CMD_RELOAD_SCENE`), and app → page
(`BRIDGE_CUE`). Names never cross as
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
the matching constants `handshake::VERSION` in `renderer/src/bridge.rs` and
`LAYOUT.handshake.version` in `dom/runtime/bridge.js`). On a mismatch the
renderer and the page refuse to attach and say so on the page, instead of drawing garbage. The
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

To match a different scene, change the names at the top of
`app/src/game/game.c` (`BALL_NODE`, `PLAYER_VARIANT`, `TABLE_COLLIDER`). The
native runs and tests do not load the bundle; `game_headless_scene()` in
`app/src/game/headless.c` describes the bundled scene's colliders by hand, so
update it if you want the tests to match your scene.

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
* **The sound player has the demo's voices.** `audio/` is runtime code, but
  its `Audio` class has one method per demo sound (`land`, `wallHit`,
  `ballClack`, `roll`); a sound of your own needs a method there.

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
