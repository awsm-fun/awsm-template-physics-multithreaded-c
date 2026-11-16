//! The render worker's life: start the renderer, load the scene, then draw a
//! frame per display refresh from whatever the app has published.
//!
//! Everything here runs on one thread, in three kinds of entry:
//!
//! * a message from the page ([`init`], [`resize`], [`anti_alias`],
//!   [`attach`], [`detach`]);
//! * the asynchronous start-up that `init` kicks off ([`App::boot`]);
//! * the `requestAnimationFrame` callback ([`on_frame`]), once start-up is
//!   done;
//! * an anti-aliasing change being applied ([`apply_anti_aliasing`]), which
//!   is asynchronous too.
//!
//! The `AwsmRenderer` itself is only ever touched from the frame callback (and
//! from start-up, before the first frame, and while an anti-aliasing change
//! has it away from the frame loop). A message from the page changes what
//! surrounds it — the canvas size, which app is attached, the anti-aliasing
//! wanted — and anything that needs the renderer, such as an `attach`, waits
//! for the next frame.

use std::cell::RefCell;
use std::collections::VecDeque;

use awsm_renderer::anti_alias::AntiAliasing;
use awsm_renderer::core::renderer::{AwsmRendererWebGpuBuilder, DeviceRequestLimits};
use awsm_renderer::debug::{AwsmRendererLogging, TimingTier};
use awsm_renderer::web_global;
use awsm_renderer::{AwsmRenderer, AwsmRendererBuilder, RendererLoadingPhase};
use awsm_renderer_scene_loader::basis::{configure, BasisWorkerConfig};
use js_sys::SharedArrayBuffer;
use wasm_bindgen::prelude::*;
use web_sys::{DedicatedWorkerGlobalScope, GpuAdapter, OffscreenCanvas};

use crate::bridge::{self, Bridge, Command, Record};
use crate::camera;
use crate::page;
use crate::scene::{self, SceneIndex};
use crate::slots::{self, Slots};

/// The anti-aliasing the page's Settings ask for. The page offers Off, SMAA
/// and 4× MSAA, never both at once: on top of an MSAA resolve SMAA gains
/// little, and it costs a full-screen pass of its own. It is the cheap option
/// for when MSAA is too expensive.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AntiAliasMode {
    pub msaa: bool,
    pub smaa: bool,
}

impl AntiAliasMode {
    /// The renderer's configuration for this mode. 4 is the only MSAA sample
    /// count the renderer supports; mipmapping is always on.
    fn config(self) -> AntiAliasing {
        AntiAliasing {
            msaa_sample_count: self.msaa.then_some(4),
            smaa: self.smaa,
            mipmap: true,
        }
    }
}

/// WebGPU guarantees every device at least this largest 2D texture size. It is
/// what the page is told when the adapter cannot be asked.
const MIN_MAX_TEXTURE_DIMENSION_2D: u32 = 8192;

/// The awsm-renderer release the vendored Basis transcoder files came with.
/// It is appended to their URLs as `?v=…`: those files keep their names from
/// one release to the next, so after a deployed upgrade a browser could
/// otherwise pair a cached old transcoder with the new worker script. Bump it
/// together with the awsm-renderer version in Cargo.toml.
const BASIS_VERSION: &str = "0.42.0";

/// Frames drawn before the page is told the scene is on screen. The very
/// first frames after a load still have uploads settling; this is when the
/// picture is actually there, and the loading overlay can go.
const FRAMES_BEFORE_READY: u32 = 3;

/// What the page's messages have asked for, and the running renderer.
#[derive(Default)]
struct State {
    /// The canvas handed over by `init`. Kept here so `resize` works at any
    /// time, including while the scene is still loading.
    canvas: Option<OffscreenCanvas>,
    /// The latest `attach` that has not been acted on yet.
    pending_attach: Option<AttachRequest>,
    /// The latest anti-aliasing change that has not been applied yet.
    pending_anti_alias: Option<AntiAliasMode>,
    /// Set while an anti-aliasing change is being applied. `app` is `None`
    /// for that time: the change owns the renderer across its awaits.
    reconfiguring: bool,
    /// A `detach` that arrived while `reconfiguring`, to be acted on when the
    /// renderer comes back.
    detach_while_reconfiguring: bool,
    /// `None` until start-up has finished, and while `reconfiguring`.
    app: Option<App>,
}

struct AttachRequest {
    memory: SharedArrayBuffer,
    bridge_ptr: u32,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());

    /// The one `requestAnimationFrame` callback, created on first use and
    /// handed to the browser again after every frame.
    static FRAME_CALLBACK: Closure<dyn FnMut(f64)> = Closure::new(on_frame);

    /// This worker's global object, looked up once. (The renderer's own
    /// `request_animation_frame` helper works out whether it is on a page or
    /// in a worker on every call, which costs a caught exception per frame
    /// here. This module always runs in a dedicated worker.)
    static WORKER_SCOPE: DedicatedWorkerGlobalScope = js_sys::global().unchecked_into();
}

/// `{type: 'init'}`: start rendering to `canvas`. `base_url` is the absolute
/// URL of the site root; a worker cannot rely on relative URLs resolving
/// against the page.
pub fn init(
    canvas: OffscreenCanvas,
    base_url: String,
    width: u32,
    height: u32,
    anti_alias: AntiAliasMode,
) {
    let first = STATE.with(|state| {
        let mut state = state.borrow_mut();
        if state.canvas.is_some() {
            return false;
        }
        state.canvas = Some(canvas.clone());
        true
    });
    if !first {
        page::post_error("the renderer was sent `init` more than once");
        return;
    }
    resize(width, height);

    let base_url = if base_url.ends_with('/') {
        base_url
    } else {
        format!("{base_url}/")
    };
    wasm_bindgen_futures::spawn_local(async move {
        match App::boot(canvas, &base_url, anti_alias).await {
            Ok(app) => {
                STATE.with(|state| state.borrow_mut().app = Some(app));
                request_frame();
            }
            Err(message) => page::post_error(&message),
        }
    });
}

/// `{type: 'resize'}`: the canvas's backing store should be `width` × `height`
/// device pixels. The renderer reads the surface size back every frame, so
/// resizing the canvas is all there is to do.
pub fn resize(width: u32, height: u32) {
    // A canvas with a zero side has no surface to draw to. Keep the last good
    // size until the page reports a real one.
    if width == 0 || height == 0 {
        return;
    }
    STATE.with(|state| {
        if let Some(canvas) = &state.borrow().canvas {
            // Assigning a dimension discards the current drawing buffer even
            // when the value is unchanged, so only assign on a real change.
            if canvas.width() != width {
                canvas.set_width(width);
            }
            if canvas.height() != height {
                canvas.set_height(height);
            }
        }
    });
}

/// `{type: 'anti-alias'}`: the page's Settings changed the anti-aliasing. It
/// is applied by the next frame (or once start-up has finished); a newer
/// change replaces one that has not been applied yet.
pub fn anti_alias(mode: AntiAliasMode) {
    STATE.with(|state| state.borrow_mut().pending_anti_alias = Some(mode));
}

/// `{type: 'attach'}`: an app instance is running; its bridge block is at
/// byte offset `bridge_ptr` of `memory`. May arrive before the scene has
/// loaded, and again whenever the app is replaced.
pub fn attach(memory: JsValue, bridge_ptr: u32) {
    match memory.dyn_into::<SharedArrayBuffer>() {
        Ok(memory) => STATE.with(|state| {
            state.borrow_mut().pending_attach = Some(AttachRequest { memory, bridge_ptr });
        }),
        Err(_) => page::post_error(
            "attach: `memory` is not a SharedArrayBuffer (is the page cross-origin isolated?)",
        ),
    }
}

/// `{type: 'detach'}`: the app is going away. Stop touching its memory; the
/// scene stays on screen as it last was.
pub fn detach() {
    STATE.with(|state| {
        let mut state = state.borrow_mut();
        state.pending_attach = None;
        if let Some(app) = state.app.as_mut() {
            app.detach();
        } else if state.reconfiguring {
            state.detach_while_reconfiguring = true;
        }
    });
}

fn request_frame() {
    FRAME_CALLBACK.with(|callback| {
        let function = callback.as_ref().unchecked_ref();
        let requested = WORKER_SCOPE.with(|scope| scope.request_animation_frame(function));
        if let Err(err) = requested {
            page::post_error(&format!("requestAnimationFrame failed: {err:?}"));
        }
    });
}

/// The `requestAnimationFrame` callback. `frame_ms` is the browser's timestamp
/// for the frame being prepared.
fn on_frame(frame_ms: f64) {
    STATE.with(|state| {
        let mut state = state.borrow_mut();
        // An anti-aliasing change recompiles pipelines, which takes awaits.
        // The renderer is handed to it for that time, so no frame is drawn
        // (and the app, which is woken by drawn frames, waits too).
        if let Some(mode) = state.pending_anti_alias {
            if let Some(app) = state.app.take() {
                state.pending_anti_alias = None;
                state.reconfiguring = true;
                wasm_bindgen_futures::spawn_local(apply_anti_aliasing(app, mode));
                return;
            }
        }
        let State {
            app,
            pending_attach,
            ..
        } = &mut *state;
        if let Some(app) = app {
            app.frame(frame_ms, pending_attach);
        }
    });
    request_frame();
}

/// Applies an anti-aliasing change to the running renderer, then hands it
/// back to the frame loop. Only the first switch to a given mode really
/// compiles anything; the renderer keeps the pipelines of modes it has used,
/// so switching back is quick.
async fn apply_anti_aliasing(mut app: App, mode: AntiAliasMode) {
    page::post_aa_compile_start(mode.msaa, mode.smaa);
    let applied = match app.renderer.set_anti_aliasing(mode.config()).await {
        // Changing the configuration only marks what has to be rebuilt; the
        // compiling happens in `commit_load`, whose progress the page shows.
        Ok(()) => {
            let mut last_line = String::new();
            app.renderer
                .commit_load(|stats| {
                    let Some(line) = stats.phase_label() else {
                        return;
                    };
                    if line != last_line {
                        page::post_aa_compile_progress(&line);
                        last_line = line;
                    }
                })
                .await
                .map(|_| ())
                .map_err(|err| format!("anti-aliasing pipelines: {err}"))
        }
        Err(err) => Err(format!("anti-aliasing change: {err}")),
    };
    match applied {
        Ok(()) => tracing::info!(
            "anti-aliasing applied (msaa {}, smaa {})",
            mode.msaa,
            mode.smaa
        ),
        Err(message) => page::post_error(&message),
    }
    // Always, even after a failure: the page lowers its overlay on this.
    page::post_aa_compile_done();

    STATE.with(|state| {
        let mut state = state.borrow_mut();
        if std::mem::take(&mut state.detach_while_reconfiguring) {
            app.detach();
        }
        state.reconfiguring = false;
        state.app = Some(app);
    });
}

/// Asks the GPU the two things the page needs to size the canvas, and posts
/// them: whether the adapter is a software fallback, and its largest 2D
/// texture. This is a separate, lightweight adapter request, made before the
/// renderer builds its own device. If it fails, the page gets the safe
/// defaults (not a fallback, the minimum every WebGPU device supports).
async fn report_gpu_info(gpu: &web_sys::Gpu) {
    let adapter = gpu
        .request_adapter()
        .await
        .ok()
        .filter(|adapter| !adapter.is_null() && !adapter.is_undefined())
        .map(|adapter| adapter.unchecked_into::<GpuAdapter>());
    let (is_fallback, max_texture_dim) = match adapter {
        Some(adapter) => (
            adapter.info().is_fallback_adapter(),
            adapter.limits().max_texture_dimension_2d(),
        ),
        None => (false, MIN_MAX_TEXTURE_DIMENSION_2D),
    };
    tracing::info!("gpu: fallback adapter {is_fallback}, largest 2D texture {max_texture_dim}");
    page::post_gpu_info(is_fallback, max_texture_dim);
}

/// The running renderer and everything that feeds it.
struct App {
    renderer: AwsmRenderer,
    /// For measuring how long a frame's work takes.
    performance: web_sys::Performance,
    scene: SceneIndex,
    slots: Slots,
    /// The app's bridge block. `None` while no app is attached.
    bridge: Option<Bridge>,
    /// `performance.timeOrigin`: added to a frame's timestamp, it puts the
    /// frame on the frame clock the app and the page also use (bridge.h,
    /// "Frame pacing").
    time_origin_ms: f64,
    /// The frame-clock time this renderer stored for the attached app at the
    /// start of the previous frame. This frame draws one step before it.
    /// `None` right after attaching.
    published_frame_us: Option<u32>,
    /// Scene records still to be handed to the app. They are queued here and
    /// moved into the ring as it has room, so a scene with more colliders than
    /// the ring holds is still described completely.
    outbox: VecDeque<Record>,
    /// Whether the page has been told about the current attach.
    attach_reported: bool,
    /// The camera in use: the app's latest valid one, or the start-up view.
    camera: bridge::Camera,
    frames_drawn: u32,
    /// The renderer's GPU frame-time sample count last published to the
    /// bridge. Timestamp readbacks arrive a few frames late, so the totals are
    /// only written when a new sample has come in.
    gpu_samples_published: Option<u64>,
    /// The previous frame's `requestAnimationFrame` timestamp, for the frame
    /// span the page's stats panel turns into missed frames.
    last_frame_ms: Option<f64>,
    /// Set while frames are failing, so a persistent failure is reported once
    /// rather than sixty times a second.
    render_failing: bool,
    warned_unknown_record: bool,
}

impl App {
    /// Creates the renderer on `canvas` and loads the scene into it.
    async fn boot(
        canvas: OffscreenCanvas,
        base_url: &str,
        anti_alias: AntiAliasMode,
    ) -> Result<Self, String> {
        let gpu = web_global::navigator_gpu()
            .ok_or("WebGPU is not available in this browser (the worker has no navigator.gpu)")?;
        let performance =
            web_global::performance().ok_or("the worker has no `performance` object")?;

        // Compressed textures are transcoded in a helper worker. The loader
        // does not know where the page serves that worker and its transcoder
        // module from, so it has to be told before the first load.
        configure(BasisWorkerConfig::player(
            format!("{base_url}workers/basis-worker.js?v={BASIS_VERSION}"),
            format!("{base_url}vendor/basis/basis_transcoder.js?v={BASIS_VERSION}"),
        ));

        page::post_progress("render worker: requesting WebGPU device…");
        // First, so the page can settle the canvas size before the slow part.
        report_gpu_info(&gpu).await;
        let gpu_builder = AwsmRendererWebGpuBuilder::new_with_offscreen_canvas(gpu, canvas)
            .with_device_request_limits(DeviceRequestLimits::max_all());
        let mut renderer = AwsmRendererBuilder::new(gpu_builder)
            // Built with the page's anti-aliasing from the start, so start-up
            // compiles exactly the pipelines this session uses.
            .with_anti_aliasing(anti_alias.config())
            // GPU timestamp queries, folded into one whole-frame time for the
            // page's stats panel ("gpu time"). Cheap, and silently off on a
            // device without `timestamp-query`.
            .with_logging(AwsmRendererLogging {
                gpu: TimingTier::Frame,
                ..Default::default()
            })
            .with_phase_handler(|phase| {
                if matches!(phase, RendererLoadingPhase::Init) {
                    page::post_progress("renderer init: GPU device + core resources…");
                }
            })
            .build()
            .await
            .map_err(|err| format!("could not create the renderer: {err}"))?;
        page::post_progress(&format!(
            "render worker: WebGPU device + renderer ready (msaa {}, smaa {})",
            anti_alias.msaa, anti_alias.smaa
        ));

        let scene = scene::load(&mut renderer, base_url).await?;
        page::post_progress("starting render loop…");

        Ok(Self {
            renderer,
            time_origin_ms: performance.time_origin(),
            performance,
            scene,
            slots: Slots::new(),
            bridge: None,
            published_frame_us: None,
            outbox: VecDeque::new(),
            attach_reported: false,
            camera: camera::STARTUP,
            frames_drawn: 0,
            gpu_samples_published: None,
            last_frame_ms: None,
            render_failing: false,
            warned_unknown_record: false,
        })
    }

    /// One display frame.
    fn frame(&mut self, frame_ms: f64, pending_attach: &mut Option<AttachRequest>) {
        let work_start = self.performance.now();

        // The app is told about the scene only once the scene is on screen.
        if self.frames_drawn >= FRAMES_BEFORE_READY {
            if let Some(request) = pending_attach.take() {
                self.attach(request);
            }
        }

        // The moment this frame shows: one step before the frame time stored
        // at the start of the previous frame. The app's burst for that frame
        // time ended on a step at or after it (`frame time - leftover`, with
        // the leftover under one step), so the two steps around it are
        // published by now unless the app is late. Fixed before the app is
        // woken below, so its next burst cannot move it.
        let mut display_us = None;
        let mut late = false;
        if let Some(bridge) = self.bridge.as_ref() {
            let step_us = (1e6 / f64::from(bridge.step_hz())).round() as u32;
            display_us = self
                .published_frame_us
                .map(|frame_us| frame_us.wrapping_sub(step_us));
            // Then start the app on this frame's steps right away, so its
            // burst has the whole frame to run in. Wrapped to 32 bits.
            let frame_us = ((self.time_origin_ms + frame_ms) * 1000.0).round() as u64 as u32;
            bridge.begin_frame(frame_us);
            self.published_frame_us = Some(frame_us);
        }

        if let Some(bridge) = self.bridge.as_mut() {
            // Hand over as much of the scene description as the ring takes.
            while let Some(record) = self.outbox.front() {
                if !bridge.try_send(record) {
                    break;
                }
                self.outbox.pop_front();
            }
            if self.outbox.is_empty() && !self.attach_reported {
                self.attach_reported = true;
                page::post_attached();
            }

            // Act on what the app asked for since the last frame.
            while let Some(record) = bridge.receive() {
                // The newest step is read after the record on purpose. The
                // app keeps running while this frame is prepared, and a
                // command's step stamp is only meaningful next to a step
                // count at least as recent as the command itself.
                let at_step = slots::effect_step(record.step(), bridge.latest_step());
                match record.command() {
                    Command::Bind {
                        slot,
                        node,
                        variant,
                    } => self.slots.bind(
                        &mut self.renderer,
                        &self.scene,
                        slot,
                        node,
                        variant,
                        at_step,
                    ),
                    Command::Spawn {
                        slot,
                        node,
                        variant,
                    } => self.slots.spawn(
                        &mut self.renderer,
                        &self.scene,
                        slot,
                        node,
                        variant,
                        at_step,
                    ),
                    Command::Despawn { slot } => {
                        self.slots
                            .despawn(&mut self.renderer, &self.scene, slot, at_step)
                    }
                    Command::Unknown { kind } => {
                        if !self.warned_unknown_record {
                            self.warned_unknown_record = true;
                            tracing::warn!(
                                "skipping app-to-renderer records of unknown type {kind} \
                                 (is the renderer older than the app?)"
                            );
                        }
                    }
                }
            }

            // Move everything to that moment: poses interpolated between the
            // two steps around it. The camera is taken whole from the newer
            // one: the app applies a page frame's input in the first step of
            // a burst, and the newer step always includes it, so every frame
            // shows exactly one page frame's turn of the camera.
            let (sample, held) = bridge.sample_at(display_us);
            late = held;
            self.slots.apply(&mut self.renderer, &self.scene, &sample);
            let published_camera = camera::newest_usable(
                sample.older.map(|snapshot| snapshot.camera()),
                sample.newer.map(|snapshot| snapshot.camera()),
            );
            if let Some(published_camera) = published_camera {
                self.camera = published_camera;
            }
        }

        // The camera is set every frame, even when it has not moved: this is
        // where the renderer picks up the surface's current aspect ratio.
        let (view, params) = camera::to_renderer(&self.camera);
        let drawn = self
            .renderer
            .set_camera(view, params)
            .map_err(|err| err.to_string())
            .and_then(|()| {
                // Resolves the transforms written above into world matrices
                // and refreshes everything derived from them (bounds, culling,
                // light and shadow bookkeeping).
                self.renderer.update_transforms();
                self.renderer.render(None).map_err(|err| err.to_string())
            });
        match drawn {
            Ok(()) => {
                self.render_failing = false;
                self.frames_drawn = self.frames_drawn.saturating_add(1);
                if self.frames_drawn == FRAMES_BEFORE_READY {
                    page::post_ready();
                }
            }
            Err(message) => {
                if !self.render_failing {
                    self.render_failing = true;
                    page::post_error(&format!("drawing a frame failed: {message}"));
                }
            }
        }

        // The frame's stats. (The app was already woken at the start of the
        // frame, whether or not the frame could be drawn: a rendering problem
        // should not also freeze the simulation.)
        if let Some(bridge) = &self.bridge {
            if let Some(gpu) = awsm_renderer::profiling::gpu_frame_timing() {
                if self.gpu_samples_published != Some(gpu.count) {
                    self.gpu_samples_published = Some(gpu.count);
                    // Both wrap at 2^32, like every counter in the stats.
                    bridge.set_gpu_totals((gpu.total_ms * 1000.0) as u64 as u32, gpu.count as u32);
                }
            }
            let work_us = (self.performance.now() - work_start) * 1000.0;
            // Rounded per frame; the page divides whole seconds of these by a
            // refresh period, so sub-microsecond loss does not matter.
            let span_us = self
                .last_frame_ms
                .map_or(0.0, |last| ((frame_ms - last) * 1000.0).round());
            bridge.frame_presented(
                work_us.max(0.0) as u32,
                span_us.clamp(0.0, u32::MAX as f64) as u32,
                late,
            );
        }
        self.last_frame_ms = Some(frame_ms);
    }

    /// Connects to an app instance's bridge block, replacing any previous one.
    fn attach(&mut self, request: AttachRequest) {
        // Whatever happens next, the previous app's memory is not read again.
        self.detach();

        let bridge = match Bridge::attach(&request.memory, request.bridge_ptr) {
            Ok(bridge) => bridge,
            Err(message) => {
                page::post_error(&format!("cannot attach to the app: {message}"));
                return;
            }
        };

        // A new app instance knows nothing of what the previous one set up, so
        // it gets the scene exactly as it was loaded.
        self.slots
            .reset(&mut self.renderer, &self.scene, bridge.pose_capacity());
        // The new app has published nothing for any earlier frame time.
        self.published_frame_us = None;
        // The new block's GPU totals start at zero; publish ours on the next
        // frame rather than waiting for the next sample.
        self.gpu_samples_published = None;

        // Describe the scene to the app: BEGIN, every collider, READY.
        self.outbox.push_back(Record::scene_begin());
        self.outbox
            .extend(self.scene.colliders.iter().map(Record::scene_collider));
        self.outbox
            .push_back(Record::scene_ready(self.scene.colliders.len() as u32));
        self.attach_reported = false;

        self.bridge = Some(bridge);
    }

    /// Forgets the attached app. Everything it was driving stays where it is.
    fn detach(&mut self) {
        self.bridge = None;
        self.outbox.clear();
    }
}
