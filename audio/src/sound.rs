//! Plays the awsm-audio project and drives it from the app's cues.
//!
//! WebAudio (and thus [`awsm_audio_player::Player`]) is main-thread only, so
//! this runs on the page, not in a worker. The app decides *when* the ball
//! rolls / hits a wall / lands and how loud (it owns the contacts); it sends
//! cues through the bridge, the page drains them and calls the methods on
//! [`Sound`] (through the `Audio` class in `lib.rs`), and those turn each one
//! into live WebAudio changes.
//!
//! **One `Player`, one `AudioContext`, many concurrent voices.** The SFX share
//! the player's master bus:
//!   * **roll** — a sustaining DSP-worklet rumble. It is the player's
//!     persistent `play()` instance (looping), so it is never cut; loudness,
//!     timbre and 3D position are driven continuously with `set_param_live`.
//!   * **wall hit** / **land** / **ball clack** — one-shots fired as
//!     independent **voices** (`play_voice_with`). A voice does not stop the
//!     roll or each other, and its per-trigger statics (intensity → gain,
//!     hardness → filter cutoff or worklet intensity, table position → stage
//!     panner) are baked in as build-time **overrides**, so the sound is
//!     correct from its very first sample. A spent one-shot self-decays to
//!     silence but its source nodes (oscillators) keep running until freed,
//!     so we [`stop_voice`] each once its tail has finished — otherwise they
//!     pile up into a constant hum. A `max_voices` cap is the backstop.
//!
//! [`stop_voice`]: awsm_audio_player::Player::stop_voice
//!
//! The export ships `.wav` bounces too, but these SFX are synthesized live, so
//! the only assets fetched are the worklet `.wasm` modules.
//!
//! ## Live controls (what the game drives at runtime)
//!
//! | sound | node (label)        | param         | driven by                          |
//! |-------|---------------------|---------------|------------------------------------|
//! | roll  | worklet             | `speed`       | normalized roll speed 0..1         |
//! | roll  | `roll_LEVEL`        | `gain`        | roll speed (0 ⇒ silent at rest)    |
//! | roll  | `roll_PANNER`       | `positionXYZ` | ball position → audio stage        |
//! | hit   | `hit_LEVEL`         | `gain`        | impact intensity 0..1              |
//! | hit   | `hit_FILTER`        | `frequency`   | impact intensity (harder⇒brighter) |
//! | hit   | `hit_PANNER`        | `positionXYZ` | impact position → audio stage      |
//! | land  | `land_LEVEL`        | `gain`        | landing intensity 0..1             |
//! | land  | `land_FILTER`       | `frequency`   | landing intensity                  |
//! | land  | `land_PANNER`       | `positionXYZ` | landing position → audio stage     |
//! | clack | worklet             | `intensity`   | collision intensity 0..1           |
//! | clack | `clack_LEVEL`       | `gain`        | collision intensity 0..1           |
//! | clack | `clack_PANNER`      | `positionXYZ` | collision position → audio stage   |
//!
//! The roll's columns are nudged live (`set_param_live`); the impacts' are
//! passed as `play_voice_with` overrides at trigger time. All the per-sound
//! worklet/DSP knobs (`roughness`, `body_hz`, `brightness`, impact mode
//! tunings, …) keep their authored values; only the columns above are touched
//! at runtime. The node labels are how this code finds its controls, so keep
//! them when you edit the project in the awsm-audio editor.
//!
//! ## Spatialization: an orbiting listener over a tabletop stage
//!
//! The listener is NOT placed at the literal (far-away, steeply tilted)
//! camera — from 9 units back the whole table subtends a narrow cone and the
//! pan goes mushy. Instead, sources sit at the ball's **world (x, z) on the
//! felt** ([`stage_pos`]), and a **virtual listener hovers low over the
//! table**, orbited around the table centre at the camera's azimuth (yaw) and
//! looking inward — see [`listener`]. We borrow only the camera's *heading*,
//! never its distance or pitch, so rail-to-rail still pans near-hard L/R while
//! the stereo image tracks the view from any angle. The page re-sends the
//! listener whenever the app reports a new camera yaw, via
//! `set_listener_live`, which moves the real `AudioListener` — so the engine
//! re-spatializes **every ringing voice**, and a one-shot fired before you
//! orbit swings correctly as you move. The ball's own y is ignored — it only
//! sounds while on the surface.

use awsm_audio_player::{Player, VoiceHandle};
use awsm_audio_schema::{Graph, NodeId, NodeKind, SampleLibrary};
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use wasm_bindgen_futures::JsFuture;

/// The tabletop's world half-extents in x and z, kept in sync by hand with the
/// scene's `Tabletop_Collider` (`media/bundle/scene.toml`: centred on the
/// origin, `half_extents = [3.0, 0.1, 2.0]`). Source positions are clamped to
/// these so a stray body cannot drag its sound off the stage.
const TABLE_HALF_X: f32 = 3.0;
const TABLE_HALF_Z: f32 = 2.0;

/// The virtual listener's orbit around the table centre (see [`listener`] and
/// the module docs): it circles at the camera's yaw, `RADIUS` out toward the
/// camera and `HEIGHT` up, looking horizontally inward. Deliberately **tight
/// and low** — down in the thick of the table, NOT out at the real camera —
/// so a rail-side ball is ~70–75° off-axis (near-hard L/R) and near/far reads
/// as a clear loudness step (near rail ≲ the panner's 1.5 `ref_distance`, far
/// rail ~3, far corners ~4.3 — a 2:1 near/far gain ratio).
const LISTENER_ORBIT_RADIUS: f32 = 1.0;
const LISTENER_HEIGHT: f32 = 0.9;

/// Cap on simultaneous one-shot impact voices. A spent one-shot keeps its
/// (idle) nodes until stopped or stolen, so this bounds the pool: once
/// exceeded, the oldest (already silent) voice is reclaimed. The roll is the
/// `play()` instance, not a voice, so it is never affected. We free voices on
/// time (see [`IMPACT_VOICE_TTL`]), so this is just a backstop.
const MAX_IMPACT_VOICES: usize = 8;

/// How long after firing an impact voice we free it. The impacts finish within
/// ~0.5 s (the longest tail is the landing's body noise); past that a spent
/// voice only holds its oscillators at the envelope's silent floor (~−62 dB),
/// so stopping it reclaims the nodes and — crucially — stops those held tones
/// from summing across voices into an audible hum. Generous margin over the
/// tail.
const IMPACT_VOICE_TTL: f64 = 0.6;

/// One SFX's graph + the control nodes we drive. The graph is cloned out of
/// the library so playing it needs no borrow of the library (and no
/// per-trigger lookup).
struct VoiceSpec {
    graph: Graph,
    level: NodeId,        // output-trim gain → loudness
    panner: NodeId,       // 3D position
    tone: Option<NodeId>, // roll/clack: the worklet; hit/land: the filter (`frequency`)
}

/// The single player + the SFX specs.
pub struct Sound {
    player: Player,
    roll: VoiceSpec,
    hit: VoiceSpec,
    land: VoiceSpec,
    /// Ball↔ball collision (modal steel-sphere worklet, `sfx_ball_clack`).
    /// `None` when the loaded export predates the sound — ball↔ball hits then
    /// fall back to the wall knock, so older exports keep working.
    clack: Option<VoiceSpec>,
    /// Set once a user gesture has resumed the AudioContext + started the roll.
    started: bool,
    /// Live impact voices awaiting cleanup: `(handle, AudioContext time to
    /// free at)`. Reaped on every cue (see [`IMPACT_VOICE_TTL`]).
    pending: Vec<(VoiceHandle, f64)>,
}

impl Sound {
    /// Fetch + parse the export, build the player, and resolve the control
    /// nodes. Does NOT start audio — that waits for a user gesture
    /// ([`start`](Self::start)), per the browser autoplay policy.
    ///
    /// `base_url` is the directory the page is served from (not just the
    /// origin), so the site keeps working when it is deployed under a subpath.
    pub async fn load(base_url: &str) -> Result<Sound, JsValue> {
        let base = base_url.trim_end_matches('/');

        // ── Parse the project.toml into a SampleLibrary ─────────────────────
        let toml_url = format!("{base}/audio-project/project.toml");
        let text = http_text(&toml_url).await?;
        let lib = parse_library(&text)?;

        // ── Fetch EVERY worklet wasm the export ships (roll + clack today —
        // each worklet node references its module by id, so all of them must
        // be compiled + stored before any voice that uses one is built).
        // Fetched CONCURRENTLY — they are independent files, so there is no
        // reason to pay a network round trip per module in series. ──────────
        if lib.assets.wasm_modules.is_empty() {
            return Err(JsValue::from_str("audio: no wasm worklet in export"));
        }
        let modules = futures::future::try_join_all(lib.assets.wasm_modules.iter().map(|wasm| {
            let wasm_url = format!("{base}/audio-project/assets/{}.wasm", wasm.id);
            async move { Ok::<_, JsValue>((wasm.id, http_bytes(&wasm_url).await?)) }
        }))
        .await?;

        // ── One player for everything (shared context + master bus) ─────────
        let mut player = build_player(&modules).await?;
        player.set_listener(Some(listener(0.0)));
        player.set_max_voices(Some(MAX_IMPACT_VOICES));

        // ── Resolve samples + their control nodes ───────────────────────────
        let roll = voice_spec(
            &lib,
            "sfx_ball_roll",
            "roll_LEVEL",
            "roll_PANNER",
            Tone::Worklet,
        )?;
        let hit = voice_spec(
            &lib,
            "sfx_wall_hit",
            "hit_LEVEL",
            "hit_PANNER",
            Tone::Filter("hit_FILTER"),
        )?;
        let land = voice_spec(
            &lib,
            "sfx_land",
            "land_LEVEL",
            "land_PANNER",
            Tone::Filter("land_FILTER"),
        )?;
        // Lenient: older exports do not have the clack; ball↔ball falls back
        // to the wall knock rather than failing the whole audio load.
        let clack = voice_spec(
            &lib,
            "sfx_ball_clack",
            "clack_LEVEL",
            "clack_PANNER",
            Tone::Worklet,
        )
        .inspect_err(|_| {
            tracing::warn!("audio: export has no sfx_ball_clack — ball↔ball uses the knock");
        })
        .ok();

        tracing::info!("audio: loaded player (roll + hit/land voices)");
        Ok(Sound {
            player,
            roll,
            hit,
            land,
            clack,
            started: false,
            pending: Vec::new(),
        })
    }

    /// Call from a user gesture (key/click): resume the AudioContext and, the
    /// first time, start the (silent-at-rest) rolling sound.
    ///
    /// Resuming is repeated on every call, not just the first: a gesture the
    /// browser does not count as activation (Escape, for one) leaves the
    /// context suspended, and the next real one must still be able to wake
    /// it. `resume()` is harmless once the context is running.
    pub fn start(&mut self) {
        self.player.resume();
        if self.started {
            return;
        }

        // The roll is the persistent `play()` instance (looping), so impact
        // voices never cut it. Start it trimmed to silence (speed 0 at rest).
        if let Err(e) = self.player.play(&self.roll.graph, true) {
            tracing::error!("audio: play roll: {e}");
        }
        self.player
            .set_param_live(self.roll.level, "gain", 0.0, 0.0);
        if let Some(worklet) = self.roll.tone {
            self.player.set_param_live(worklet, "speed", 0.0, 0.0);
        }
        self.started = true;
        tracing::info!("audio: started (context resumed, roll playing)");
    }

    /// Continuous rolling state: normalized speed (0..1) + ball world (x, z).
    pub fn roll(&mut self, speed: f32, x: f32, z: f32) {
        if !self.before_cue() {
            return;
        }
        // Loudness tracks speed (silent at rest); the worklet's `speed` drives
        // impact density + ring length for a faster/denser rumble. These
        // nudge the live roll instance.
        let p = &self.player;
        p.set_param_live(self.roll.level, "gain", speed, 0.05);
        if let Some(worklet) = self.roll.tone {
            p.set_param_live(worklet, "speed", speed, 0.05);
        }
        let [sx, sy, sz] = stage_pos(x, z);
        set_panner_live(p, self.roll.panner, sx, sy, sz, 0.05);
    }

    /// A ball struck a rail — `intensity` (0..1) from the impact speed.
    pub fn wall_hit(&mut self, x: f32, z: f32, intensity: f32) {
        if !self.before_cue() {
            return;
        }
        tracing::debug!("audio cue: wall-hit intensity {intensity:.2}");
        // freq: harder knocks ring brighter — but ceramic-on-WOOD stays low
        // and thumpy (a measured knock has 67% of its energy below 300 Hz and
        // ~nothing past 1.2 kHz), so the filter sweeps 0.3–0.9 kHz. Keeps the
        // knock a world apart from the 2–4 kHz ball↔ball clack.
        let h = trigger(
            &mut self.player,
            &self.hit,
            intensity,
            ("frequency", 300.0 + intensity * 600.0),
            x,
            z,
        );
        self.track_voice(h);
    }

    /// A ball landed on the table — `intensity` (0..1) from the drop speed.
    pub fn land(&mut self, x: f32, z: f32, intensity: f32) {
        if !self.before_cue() {
            return;
        }
        tracing::debug!("audio cue: land intensity {intensity:.2}");
        // Lower band than a wall knock — a floor thud.
        let h = trigger(
            &mut self.player,
            &self.land,
            intensity,
            ("frequency", 250.0 + intensity * 800.0),
            x,
            z,
        );
        self.track_voice(h);
    }

    /// Two balls collided — `intensity` (0..1) from the approach speed.
    pub fn ball_clack(&mut self, x: f32, z: f32, intensity: f32) {
        if !self.before_cue() {
            return;
        }
        tracing::debug!("audio cue: ball-clack intensity {intensity:.2}");
        // The clack worklet derives everything (loudness curve, contact time,
        // spectral tilt) from the one `intensity` param; the LEVEL gain scales
        // on top for distance-ish feel.
        let h = match &self.clack {
            Some(clack) => trigger(
                &mut self.player,
                clack,
                intensity,
                ("intensity", intensity),
                x,
                z,
            ),
            // Export predates the clack — voice it as a knock.
            None => trigger(
                &mut self.player,
                &self.hit,
                intensity,
                ("frequency", 700.0 + intensity * 2800.0),
                x,
                z,
            ),
        };
        self.track_voice(h);
    }

    /// Follow the camera: move the virtual listener to the given camera yaw
    /// (see [`listener`]). `set_listener_live` writes the context's
    /// `AudioListener` position + orientation immediately, so every ringing
    /// voice — including one-shots whose panner position was baked at trigger
    /// — re-spatializes to the new heading. The app sends a new yaw as the
    /// camera orbits; the per-update deltas are small, so the un-glided write
    /// cannot zipper.
    pub fn set_camera_yaw(&mut self, yaw: f32) {
        self.player.set_listener_live(&listener(yaw));
    }

    /// Shared prologue of every gameplay cue: nothing is audible before the
    /// first gesture, and once started, every cue first frees the impact
    /// voices whose tail has finished. The roll cue arrives continuously
    /// (~20 Hz, even at rest), so this reaps promptly without a separate
    /// timer. Returns whether the cue should be applied.
    fn before_cue(&mut self) -> bool {
        if !self.started {
            return false;
        }
        self.reap_voices();
        true
    }

    /// Queue a freshly fired impact voice for cleanup once its tail has
    /// decayed.
    fn track_voice(&mut self, handle: Option<VoiceHandle>) {
        if let Some(h) = handle {
            self.pending
                .push((h, self.player.current_time() + IMPACT_VOICE_TTL));
        }
    }

    /// Stop every impact voice whose free-at time has passed, reclaiming its
    /// (otherwise endlessly running) source nodes.
    fn reap_voices(&mut self) {
        let now = self.player.current_time();
        let mut i = 0;
        while i < self.pending.len() {
            if self.pending[i].1 <= now {
                let (handle, _) = self.pending.remove(i);
                self.player.stop_voice(handle);
            } else {
                i += 1;
            }
        }
    }
}

/// Which "tone" control a voice exposes.
enum Tone {
    Worklet,
    Filter(&'static str),
}

/// Fire a one-shot impact as an independent voice. Its per-hit statics —
/// loudness, tone, and 3D position — are passed as build-time **overrides**,
/// so the sound is spatialized + shaped correctly from its first sample (no
/// render-quantum catch-up). `tone` is the voice's tone-control override —
/// `("frequency", hz)` for the filter voices, `("intensity", 0..1)` for the
/// clack worklet. The voice runs alongside the roll and the other impacts; the
/// player's `max_voices` cap reclaims it once enough newer ones have fired.
fn trigger(
    player: &mut Player,
    v: &VoiceSpec,
    intensity: f32,
    tone: (&str, f32),
    x: f32,
    z: f32,
) -> Option<VoiceHandle> {
    let [sx, sy, sz] = stage_pos(x, z);
    let mut overrides: Vec<(NodeId, &str, f32)> = vec![
        (v.level, "gain", intensity),
        (v.panner, "positionX", sx),
        (v.panner, "positionY", sy),
        (v.panner, "positionZ", sz),
    ];
    if let Some(tone_node) = v.tone {
        overrides.push((tone_node, tone.0, tone.1));
    }
    match player.play_voice_with(&v.graph, false, &overrides) {
        Ok(handle) => Some(handle),
        Err(e) => {
            tracing::error!("audio: play impact: {e}");
            None
        }
    }
}

fn set_panner_live(p: &Player, node: NodeId, x: f32, y: f32, z: f32, glide: f64) {
    p.set_param_live(node, "positionX", x, glide);
    p.set_param_live(node, "positionY", y, glide);
    p.set_param_live(node, "positionZ", z, glide);
}

/// Map the ball's world (x, z) onto the audio stage: sources sit **at the
/// ball's world position on the felt** (y = 0), clamped inside the rails. The
/// spatial exaggeration lives entirely in the *listener* placement (tight and
/// low — see [`listener`]); keeping sources world-true is what lets the
/// listener orbit: left/right AND near/far track the view from any angle, and
/// a voice ringing when you orbit re-spatializes correctly, because positions
/// never depend on the camera. The ball's own world y is ignored — it only
/// sounds while on the surface.
fn stage_pos(x: f32, z: f32) -> [f32; 3] {
    [
        x.clamp(-TABLE_HALF_X, TABLE_HALF_X),
        0.0,
        z.clamp(-TABLE_HALF_Z, TABLE_HALF_Z),
    ]
}

/// Build the `Player`: load the worklet shim, then compile + store every
/// worklet module so the graphs that use them can instantiate.
async fn build_player(
    modules: &[(awsm_audio_schema::AssetId, Vec<u8>)],
) -> Result<Player, JsValue> {
    let mut player =
        Player::new().map_err(|e| JsValue::from_str(&format!("audio: Player::new: {e:#}")))?;
    let shim = player
        .add_worklet_shim()
        .map_err(|e| JsValue::from_str(&format!("audio: worklet shim: {e:#}")))?;
    JsFuture::from(shim).await?;
    player.mark_worklet_ready();

    for (wasm_id, wasm_bytes) in modules {
        let arr = js_sys::Uint8Array::from(wasm_bytes.as_slice());
        let module = JsFuture::from(Player::compile_module(&arr)).await?;
        player.store_module(
            *wasm_id,
            module.unchecked_into::<js_sys::WebAssembly::Module>(),
        );
    }
    Ok(player)
}

/// Resolve a voice's sample + control nodes by name/label (cloning its graph),
/// erroring loudly if the export's shape drifted from what we drive.
fn voice_spec(
    lib: &SampleLibrary,
    sample_name: &str,
    level_label: &str,
    panner_label: &str,
    tone: Tone,
) -> Result<VoiceSpec, JsValue> {
    let sample = lib
        .samples
        .iter()
        .find(|s| s.name == sample_name)
        .ok_or_else(|| JsValue::from_str(&format!("audio: no sample '{sample_name}'")))?;
    let graph = &sample.graph;
    let level = node_by_label(graph, level_label)
        .ok_or_else(|| JsValue::from_str(&format!("audio: no node '{level_label}'")))?;
    let panner = node_by_label(graph, panner_label)
        .ok_or_else(|| JsValue::from_str(&format!("audio: no node '{panner_label}'")))?;
    let tone = match tone {
        Tone::Worklet => worklet_node(graph)
            .ok_or_else(|| JsValue::from_str(&format!("audio: {sample_name} has no worklet node")))
            .map(Some)?,
        Tone::Filter(label) => node_by_label(graph, label)
            .ok_or_else(|| JsValue::from_str(&format!("audio: no node '{label}'")))
            .map(Some)?,
    };
    Ok(VoiceSpec {
        graph: graph.clone(),
        level,
        panner,
        tone,
    })
}

/// First node whose label contains `needle`.
fn node_by_label(graph: &Graph, needle: &str) -> Option<NodeId> {
    graph
        .nodes
        .iter()
        .find(|n| n.label.as_deref().is_some_and(|l| l.contains(needle)))
        .map(|n| n.id)
}

/// The graph's audio-worklet node, if any.
fn worklet_node(graph: &Graph) -> Option<NodeId> {
    graph
        .nodes
        .iter()
        .find(|n| matches!(n.kind, NodeKind::AudioWorklet(_)))
        .map(|n| n.id)
}

/// Parse `project.toml` (whose top level wraps the library in a `[library]`
/// table, alongside editor-only `pan_x`/`zoom` we ignore) into a
/// `SampleLibrary`.
fn parse_library(text: &str) -> Result<SampleLibrary, JsValue> {
    #[derive(serde::Deserialize)]
    struct Doc {
        library: SampleLibrary,
    }
    let doc: Doc = toml::from_str(text)
        .map_err(|e| JsValue::from_str(&format!("audio: parse project.toml: {e}")))?;
    Ok(doc.library)
}

/// The virtual listener at a given camera yaw: on a circle of
/// [`LISTENER_ORBIT_RADIUS`] around the table centre at the **camera's
/// azimuth**, [`LISTENER_HEIGHT`] up, looking horizontally inward. The
/// `(sinθ, cosθ)` placement matches the app's orbit camera (eye at
/// `(sin yaw, cos yaw) · r`), so the listener always sits on the camera's side
/// of the table — at yaw 0 that is (0, h, r) looking down −Z. Forward stays
/// horizontal (up +Y): pitch is a *visual* knob, and a level ear axis keeps
/// the L/R cue at full strength.
fn listener(yaw: f32) -> awsm_audio_schema::Listener {
    let (s, c) = yaw.sin_cos();
    let mut l = awsm_audio_schema::Listener::default();
    l.position_x.value = LISTENER_ORBIT_RADIUS * s;
    l.position_y.value = LISTENER_HEIGHT;
    l.position_z.value = LISTENER_ORBIT_RADIUS * c;
    l.forward_x.value = -s;
    l.forward_y.value = 0.0;
    l.forward_z.value = -c;
    // up stays (0, 1, 0) from Default.
    l
}

// Both fetch with `no-cache` (revalidate with the server, never trust the
// heuristic cache): these are RUNTIME fetches, so a normal page refresh does
// not bust them — without this, re-exporting the audio project keeps playing
// the browser's stale copy no matter how often you reload.

async fn http_text(url: &str) -> Result<String, JsValue> {
    http_get(url)
        .await?
        .text()
        .await
        .map_err(|e| JsValue::from_str(&format!("audio: read {url}: {e}")))
}

async fn http_bytes(url: &str) -> Result<Vec<u8>, JsValue> {
    http_get(url)
        .await?
        .binary()
        .await
        .map_err(|e| JsValue::from_str(&format!("audio: read {url}: {e}")))
}

/// A `no-cache` GET that fails on an HTTP error status. Without the status
/// check a missing file (say, `task media:build` never ran) would surface as
/// a confusing TOML parse error or wasm compile error about the 404 page.
async fn http_get(url: &str) -> Result<gloo_net::http::Response, JsValue> {
    let response = gloo_net::http::Request::get(url)
        .cache(web_sys::RequestCache::NoCache)
        .send()
        .await
        .map_err(|e| JsValue::from_str(&format!("audio: fetch {url}: {e}")))?;
    if !response.ok() {
        return Err(JsValue::from_str(&format!(
            "audio: fetch {url}: HTTP {}",
            response.status()
        )));
    }
    Ok(response)
}
