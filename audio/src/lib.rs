//! The page's sound.
//!
//! This project runs on three threads that share one block of memory (see
//! `app/src/bridge.h`). Sound belongs to none of the workers: WebAudio exists
//! only on the main thread, so this crate is a plain wasm-bindgen module that
//! `dom/main.js` imports next to its own code. It knows nothing about the
//! bridge. The page drains the app's cues and calls the methods of [`Audio`];
//! this module turns them into WebAudio changes with `awsm-audio-player`.
//!
//! # Map
//!
//! * this file — the JavaScript API (the [`Audio`] class) and logging.
//! * [`sound`] — loading the awsm-audio project and voicing each cue: a
//!   looping roll driven live, one-shot impact voices, and a listener that
//!   follows the camera's heading.
//!
//! # From JavaScript
//!
//! ```js
//! import loadAudio, { Audio } from './audio/audio.js';
//! await loadAudio();                       // instantiate this wasm module
//! const audio = await Audio.load(baseUrl); // fetch + compile the project
//! audio.start();                           // in a key / pointer handler
//! audio.roll(speed, x, z);                 // ~20 Hz while the scene runs
//! audio.wallHit(x, z, intensity);          // one-shots, intensity 0..1
//! audio.land(x, z, intensity);
//! audio.ballClack(x, z, intensity);
//! audio.setCameraYaw(yaw);                 // radians, whenever it changes
//! ```

mod sound;

use wasm_bindgen::prelude::*;

/// Runs once when the wasm module is instantiated.
#[wasm_bindgen(start)]
pub fn boot() {
    // A panic aborts the module, and every later call from the page then
    // throws. The page stops calling it after the first throw; this hook makes
    // sure the console says why, instead of only "unreachable".
    std::panic::set_hook(Box::new(|info| {
        web_sys::console::error_1(&format!("audio panicked: {info}").into());
    }));
    install_tracing();
}

/// The sound of the game: one AudioContext, the rolling sound and the impact
/// voices. Create it with [`Audio::load`]; it stays silent until
/// [`Audio::start`] is called from a user gesture.
///
/// Positions are world coordinates on the table (x right, z toward the
/// default camera), intensities and speeds are 0..1. Calls made before
/// `start()` are ignored, except `setCameraYaw`, which always applies.
#[wasm_bindgen]
pub struct Audio {
    sound: sound::Sound,
}

#[wasm_bindgen]
impl Audio {
    /// Fetches `<baseUrl>/audio-project/project.toml` and the worklet modules
    /// in `<baseUrl>/audio-project/assets/`, then builds the player. `baseUrl`
    /// is the directory the page is served from. Rejects with a message if
    /// anything is missing or malformed.
    pub async fn load(base_url: String) -> Result<Audio, JsValue> {
        Ok(Audio {
            sound: sound::Sound::load(&base_url).await?,
        })
    }

    /// Resumes the AudioContext and, the first time, starts the rolling sound
    /// (silent until `roll` reports a speed). Browsers only allow audio after
    /// a user gesture, so call this from a key or pointer handler. Safe to
    /// call on every gesture.
    pub fn start(&mut self) {
        self.sound.start();
    }

    /// The player's ball rolling: `speed` 0..1 (0 is silent), at world (x, z).
    /// Glides there over 50 ms, so ~20 updates a second sound continuous.
    pub fn roll(&mut self, speed: f32, x: f32, z: f32) {
        self.sound.roll(speed, x, z);
    }

    /// A ball knocked against a rail.
    #[wasm_bindgen(js_name = wallHit)]
    pub fn wall_hit(&mut self, x: f32, z: f32, intensity: f32) {
        self.sound.wall_hit(x, z, intensity);
    }

    /// A ball landed on the table.
    pub fn land(&mut self, x: f32, z: f32, intensity: f32) {
        self.sound.land(x, z, intensity);
    }

    /// Two balls hit each other.
    #[wasm_bindgen(js_name = ballClack)]
    pub fn ball_clack(&mut self, x: f32, z: f32, intensity: f32) {
        self.sound.ball_clack(x, z, intensity);
    }

    /// The camera's orbit angle in radians, with the eye at
    /// `(sin yaw, cos yaw) · r` around the table. The listener follows it, so
    /// left and right on screen stay left and right in the ears.
    #[wasm_bindgen(js_name = setCameraYaw)]
    pub fn set_camera_yaw(&mut self, yaw: f32) {
        self.sound.set_camera_yaw(yaw);
    }
}

/// Sends `tracing` output (this crate's and the player's) to the browser
/// console, formatted like the renderer's.
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

    // INFO and up. Each impact also logs at DEBUG ("audio cue: …"); raise
    // this to LevelFilter::DEBUG to watch them arrive.
    let _ = tracing_subscriber::registry()
        .with(LevelFilter::INFO)
        .with(console)
        .try_init();
}
