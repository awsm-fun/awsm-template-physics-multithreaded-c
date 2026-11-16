//! The display cursor: which simulation time a frame shows.
//!
//! The app publishes one snapshot per fixed step, but it does not wake up at
//! perfectly even intervals: some frames find three new steps, others five,
//! and after a hitch a burst of many. Drawing "the newest step" would turn
//! that unevenness straight into visible stutter.
//!
//! So the renderer keeps its own clock. The cursor is a fractional step index
//! that advances by exactly `frame time × step rate` every frame, and trails
//! the newest published step by a small LAG. The snapshot ring is the jitter
//! buffer in between: as long as the app's unevenness is smaller than the lag,
//! it only changes how far behind the cursor is, never how fast the cursor
//! moves, and motion on screen stays uniform.
//!
//! The lag is display latency, so it should be as small as the app's wake-up
//! pattern allows. It therefore ADAPTS: it follows a slowly decaying peak of
//! the observed unevenness. A desktop sits at the minimum; a busy phone, where
//! the app is sometimes woken late and then catches up in a burst, drifts up
//! until the bursts fit, and comes back down when things calm.

/// Smallest lag, in steps. Ordinary wake-up wobble fits inside this.
const LAG_MIN: f64 = 2.0;

/// Largest lag, in steps. Beyond this, smoother motion is no longer worth the
/// added latency.
const LAG_MAX: f64 = 24.0;

/// The lag never uses more than this share of the snapshot ring, so the
/// cursor stays clear of the entries the app is about to overwrite.
const LAG_MAX_RING_SHARE: f64 = 0.75;

/// Per-frame decay of the remembered unevenness peak: it halves in about five
/// seconds at 60 frames per second.
const WOBBLE_DECAY: f64 = 0.9977;

/// How strongly one frame's measurement moves the smoothed cursor error. The
/// raw error carries the app's wake-up wobble; the thing worth correcting is
/// the slow drift underneath it.
const ERROR_SMOOTHING: f64 = 0.1;

/// Share of the smoothed error corrected per frame. Small on purpose: it only
/// has to cancel slow drift between the display clock and the step clock, and
/// a large value would feed the wobble back into the motion.
const REANCHOR_GAIN: f64 = 0.05;

/// A frame gap longer than this (a hidden tab, a debugger pause) is not
/// played back; the cursor just carries on from where it was.
const MAX_FRAME_SECONDS: f64 = 0.1;

pub struct DisplayCursor {
    /// The fractional step being displayed.
    display_step: f64,
    /// How far behind the newest step the cursor aims to be, in steps.
    target_lag: f64,
    /// Decaying peak of how far the cursor was found from its target.
    wobble: f64,
    /// Smoothed cursor error, in steps.
    smoothed_error: f64,
    /// The latest frame's cursor error before correction, in steps. Reported
    /// to the page's stats panel, as is `target_lag`.
    last_error: f64,
    /// Whether the cursor has been placed relative to a published step yet.
    anchored: bool,
    /// Timestamp of the previous frame, in milliseconds.
    last_frame_ms: Option<f64>,
}

impl DisplayCursor {
    pub fn new() -> Self {
        Self {
            display_step: 0.0,
            target_lag: LAG_MIN,
            wobble: 0.0,
            smoothed_error: 0.0,
            last_error: 0.0,
            anchored: false,
            last_frame_ms: None,
        }
    }

    /// Advances the cursor for one frame and returns the fractional step to
    /// display. The result is never ahead of `latest_step`.
    ///
    /// `frame_ms` must be the timestamp `requestAnimationFrame` passes to its
    /// callback, not `performance.now()` read inside it. Frames reach the
    /// screen on the display's refresh grid, and that timestamp is on the
    /// grid; the callback itself runs a variable millisecond or three later,
    /// and timing with "now" would bake that delay into every position.
    pub fn advance(&mut self, frame_ms: f64, latest_step: u32, step_hz: f32, ring_len: u32) -> f64 {
        let frame_seconds = match self.last_frame_ms {
            Some(previous) => ((frame_ms - previous) / 1000.0).clamp(0.0, MAX_FRAME_SECONDS),
            None => 0.0,
        };
        self.last_frame_ms = Some(frame_ms);

        let latest = f64::from(latest_step);
        let ring_len = f64::from(ring_len);
        let lag_max = LAG_MAX.min(ring_len * LAG_MAX_RING_SHARE);
        let lag_min = LAG_MIN.min(lag_max);

        self.display_step += frame_seconds * f64::from(step_hz);

        // How far the cursor is from where it aims to be.
        let error = (latest - self.target_lag) - self.display_step;
        self.last_error = error;
        if self.anchored && error.abs() < ring_len {
            // Ordinary unevenness: remember its peak, size the lag to cover
            // it, and nudge the cursor towards its target.
            self.wobble = (self.wobble * WOBBLE_DECAY).max(error.abs());
            self.target_lag = (lag_min + self.wobble).clamp(lag_min, lag_max);
            self.smoothed_error += (error - self.smoothed_error) * ERROR_SMOOTHING;
            self.display_step += self.smoothed_error * REANCHOR_GAIN;
        } else {
            // Either the first steps have only just appeared, or there was a
            // discontinuity (the tab was hidden, the app stalled). Neither is
            // unevenness to smooth over or to size the lag for: place the
            // cursor at its target and carry on from there.
            self.display_step = latest - self.target_lag;
            self.smoothed_error = 0.0;
            // Step 0 is never published (the app counts from 1), so a latest
            // step of 0 means there is nothing to anchor to yet.
            self.anchored = latest_step != 0;
        }

        // Stay inside what the ring still holds: no newer than the newest
        // step, and clear of the oldest entries, which the app overwrites
        // next.
        let oldest = (latest - (ring_len - 2.0).max(0.0)).max(0.0);
        self.display_step = self.display_step.clamp(oldest, latest);
        self.display_step
    }

    /// How far the cursor was from where it aimed to be at the latest
    /// [`advance`](Self::advance), before it was corrected, in steps. Values
    /// that stay beyond what the lag can absorb mean motion is visibly
    /// uneven.
    pub fn error(&self) -> f64 {
        self.last_error
    }

    /// How far the cursor currently aims to trail the newest step, in steps.
    pub fn lag(&self) -> f64 {
        self.target_lag
    }
}
