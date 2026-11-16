//! The render worker.
//!
//! This project runs on three threads that share one block of memory:
//!
//! * the page (`dom/`, JavaScript) owns the DOM and the input;
//! * the app (`app/`, C) owns all state and logic, and publishes what the
//!   world looks like after every fixed simulation step;
//! * this crate draws it, with `awsm-renderer`, on an `OffscreenCanvas`.
//!
//! The renderer holds no application state of its own. Each frame it copies
//! the two most relevant published steps out of the shared "bridge block",
//! interpolates between them, moves the corresponding transforms, and draws.
//!
//! It is an ordinary single-threaded wasm module. The bridge block lives in
//! the APP's memory, which this module cannot address directly, so all access
//! goes through JS typed-array views of that memory (see [`bridge`]).
//!
//! # Map
//!
//! * [`bridge`] — the wire format; the only module that knows the layout.
//! * [`render`] — start-up, the frame loop, attach / detach.
//! * [`scene`] — loading the scene, its colliders and its addressable nodes.
//! * [`slots`] — what each pose slot drives (bind / spawn / despawn).
//! * [`cursor`] — which simulation time a frame shows.
//! * [`camera`] — the app's camera, as the renderer wants it.
//! * [`page`] — messages to the page.
//!
//! Besides drawing, it reports to the page's stats panel through the bridge
//! block (frame and GPU time, how well the display keeps up with the steps)
//! and applies the page's Settings: resolution arrives as an ordinary
//! `resize`, anti-aliasing as `anti-alias`.
//!
//! # Entry points
//!
//! `web/worker.js` loads this module and forwards each message from the page
//! to the function of the same name below. The start function runs once, when
//! the module is instantiated.

mod bridge;
mod camera;
mod cursor;
mod page;
mod render;
mod scene;
mod slots;

use wasm_bindgen::prelude::*;
use web_sys::OffscreenCanvas;

/// Runs once when the wasm module is instantiated, before any message.
#[wasm_bindgen(start)]
pub fn start() {
    // A panic aborts the module. Without this hook all the page would ever
    // see is "RuntimeError: unreachable"; with it, the page gets the message.
    std::panic::set_hook(Box::new(|info| {
        page::post_error(&format!("the renderer panicked: {info}"));
    }));
    install_tracing();
}

/// `{type: 'init', canvas, baseUrl, width, height, msaa, smaa}`. `msaa` and
/// `smaa` are the anti-aliasing the page's Settings ask for, so the renderer
/// is built with it rather than switching right after start-up.
#[wasm_bindgen]
pub fn init(
    canvas: OffscreenCanvas,
    base_url: String,
    width: u32,
    height: u32,
    msaa: bool,
    smaa: bool,
) {
    render::init(
        canvas,
        base_url,
        width,
        height,
        render::AntiAliasMode { msaa, smaa },
    );
}

/// `{type: 'resize', width, height}`.
#[wasm_bindgen]
pub fn resize(width: u32, height: u32) {
    render::resize(width, height);
}

/// `{type: 'anti-alias', msaa, smaa}`: the user changed anti-aliasing in the
/// page's Settings.
#[wasm_bindgen(js_name = antiAlias)]
pub fn anti_alias(msaa: bool, smaa: bool) {
    render::anti_alias(render::AntiAliasMode { msaa, smaa });
}

/// `{type: 'attach', memory, bridgePtr}`.
#[wasm_bindgen]
pub fn attach(memory: JsValue, bridge_ptr: u32) {
    render::attach(memory, bridge_ptr);
}

/// `{type: 'detach'}`.
#[wasm_bindgen]
pub fn detach() {
    render::detach();
}

/// Sends `tracing` output (this crate's and the renderer's) to the browser
/// console.
fn install_tracing() {
    use tracing_subscriber::filter::LevelFilter;
    use tracing_subscriber::prelude::*;

    let console = tracing_subscriber::fmt::layer()
        // The console is not a terminal, and it timestamps lines itself. (The
        // default timer would also panic: wasm has no system clock.)
        .with_ansi(false)
        .without_time()
        .with_target(false)
        .with_writer(tracing_web::MakeWebConsoleWriter::new());

    // INFO and up only. Console output is slow enough that the renderer's
    // per-frame debug logging would show up as dropped frames.
    let _ = tracing_subscriber::registry()
        .with(LevelFilter::INFO)
        .with(console)
        .try_init();
}
