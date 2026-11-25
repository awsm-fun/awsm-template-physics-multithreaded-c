//! What this renderer tells the page's stats panel.
//!
//! awsm-renderer-stats does the measuring and the formatting: frame windows,
//! smoothing, the frame budget, the renderer's CPU scopes and GPU passes
//! (see its `StatsCollector`). The page only shows the report this worker
//! posts about once a second (`dom/runtime/kit/stats.js`), and sends back
//! which groups are open and what it measured itself (its refresh rate, its
//! own work).
//!
//! This file adds what is particular to this project: the names of the
//! frame's phases, and two groups read from the bridge block's running
//! totals:
//!
//! * `app`: the C app's own time per step (the step minus Box3D's part), its
//!   steps per second, the frames it was late for, and its step phases,
//!   which it times only while the group is open (`BRIDGE_DETAIL_APP`). Its
//!   frame-budget lane is the whole app worker: the app and Box3D's step.
//!   The page adds the game's own rows (`gameStatsRows` in `dom/game.js`)
//!   and the last sound cue, which only it knows.
//! * `physics`: Box3D's time per step and, while open, its stages with the
//!   parts of its solver under `solve` (`BRIDGE_DETAIL_PHYSICS`). It runs on
//!   the app worker (and its solver threads), so it has no lane of its own.

use std::collections::HashMap;

use awsm_renderer_stats::{
    duration_row, step_headline, AppSection, NamedMeans, SectionSpec, StatRow, StatsCollector,
    StatsConfig, Window,
};

use crate::bridge::{AppCounters, AppTiming, AppTimingKind, Bridge, Detail};

/// The phases of one frame, in order; the stats panel lists them by these
/// names (`PHASE_NAMES`).
#[derive(Clone, Copy)]
pub enum Phase {
    /// Everything up to the transforms being written: waking the app, the
    /// event rings, sampling and interpolating its snapshots.
    ReadApp,
    /// The scene's own motion: its clips, texture UV flows and particles.
    Animate,
    SetCamera,
    UpdateTransforms,
    /// `AwsmRenderer::render`: encoding every pass and submitting.
    Render,
}

const PHASE_NAMES: [&str; 5] = [
    "read app state + interpolate",
    "clips + particles",
    "set camera",
    "update transforms",
    "render (encode + submit)",
];

/// The C app's group: its id (the page stores whether it is open under it).
pub const APP: &str = "app";
const APP_LABEL: &str = "app (C, app worker)";
/// Its lane of the frame budget: the whole app worker.
const APP_BUDGET_LABEL: &str = "app worker (app + Box3D step)";
/// Box3D's group.
pub const PHYSICS: &str = "physics";
const PHYSICS_LABEL: &str = "physics (Box3D)";
/// Both run on the app worker: one lane colour.
const SIM_LANE: &str = "sim";
/// Box3D's stage that the parts of its solver are a breakdown of.
const SOLVE_STAGE: &str = "solve";

pub fn config() -> StatsConfig {
    StatsConfig {
        phases: PHASE_NAMES.map(String::from).to_vec(),
        render_phase: Some(Phase::Render as usize),
        sections: vec![
            SectionSpec::new(APP, APP_LABEL, Some(SIM_LANE)),
            SectionSpec::new(PHYSICS, PHYSICS_LABEL, Some(SIM_LANE)),
        ],
        ..StatsConfig::default()
    }
}

/// The breakdowns the app should time now, from the groups the page has
/// open.
pub fn detail(stats: &StatsCollector) -> Detail {
    Detail {
        app: stats.is_open(APP),
        renderer: stats.renderer_detail(),
        gpu: stats.is_open(awsm_renderer_stats::GPU),
        physics: stats.is_open(PHYSICS),
    }
}

/// The app's and Box3D's groups: the bridge totals at the last report, and
/// the breakdowns' averages.
#[derive(Default)]
pub struct AppStats {
    baseline: Option<AppCounters>,
    timings: HashMap<u32, (u32, u32)>,
    phases: NamedMeans,
    stages: NamedMeans,
    solve: NamedMeans,
}

impl AppStats {
    /// A new app instance: its counters start from zero.
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    /// The app's and Box3D's groups for one window. Without an app
    /// attached, just dashes.
    pub fn sections(
        &mut self,
        stats: &mut StatsCollector,
        window: &Window,
        bridge: Option<&Bridge>,
    ) -> Vec<AppSection> {
        let app = SectionSpec::new(APP, APP_LABEL, Some(SIM_LANE));
        let physics = SectionSpec::new(PHYSICS, PHYSICS_LABEL, Some(SIM_LANE));
        let Some(bridge) = bridge else {
            return vec![app.group().headline("—"), physics.group().headline("—")];
        };
        let now = bridge.app_counters();
        let previous = self.baseline.replace(now);
        let timings = bridge.app_timings();
        let previous_timings = std::mem::replace(
            &mut self.timings,
            timings
                .iter()
                .map(|t| (t.slot, (t.total_ns, t.count)))
                .collect(),
        );

        // The thread count is known as soon as the app reports it.
        let mut physics_group = physics.group();
        if now.solver_threads > 0 {
            let threads = now.solver_threads;
            physics_group.label = format!(
                "physics (Box3D, {threads} thread{})",
                if threads == 1 { "" } else { "s" }
            );
        }
        let Some(before) = previous else {
            return vec![app.group().headline("—"), physics_group.headline("—")];
        };

        // Wrapping totals: the change since the last report. The step in two
        // parts: Box3D's, and the rest, which is the app's own.
        let steps = now.latest_step.wrapping_sub(before.latest_step);
        let step_ms = f64::from(now.step_work_us.wrapping_sub(before.step_work_us)) / 1000.0;
        let physics_ms =
            f64::from(now.physics_step_us.wrapping_sub(before.physics_step_us)) / 1000.0;
        let steps_per_s = stats.smooth("steps", f64::from(steps) / window.seconds);
        let (own, box3d) = if steps > 0 {
            let n = f64::from(steps);
            (
                Some(stats.smooth("app", ((step_ms - physics_ms) / n).max(0.0))),
                Some(stats.smooth("physics", physics_ms / n)),
            )
        } else {
            (stats.smoothed("app"), stats.smoothed("physics"))
        };
        // The whole step per frame drawn (time per step × steps per frame):
        // what the app worker uses of a frame's budget. Box3D's part is in
        // it: the app worker runs the step, and its solver threads only help.
        let per_frame = (window.frames > 0)
            .then(|| stats.smooth("frame", step_ms / f64::from(window.frames)))
            .or(stats.smoothed("frame"));

        // Each entry's mean per step over the window. An entry that did not
        // advance is not being measured (its group was just opened) and
        // drops out.
        let means = |kind: AppTimingKind| -> Vec<(String, f64)> {
            timings
                .iter()
                .filter(|t| t.kind == kind)
                .filter_map(|t: &AppTiming| {
                    let (total0, count0) = previous_timings.get(&t.slot)?;
                    let count = t.count.wrapping_sub(*count0);
                    (count > 0).then(|| {
                        let ms =
                            f64::from(t.total_ns.wrapping_sub(*total0)) / f64::from(count) / 1e6;
                        (t.name.clone(), ms)
                    })
                })
                .collect()
        };

        let mut app_rows = vec![
            StatRow::new("steps", format!("{steps_per_s:.0} steps/s")),
            window.late.row("late frames"),
        ];
        if stats.is_open(APP) {
            self.phases.update(means(AppTimingKind::Phase));
            app_rows.extend(
                self.phases
                    .entries()
                    .iter()
                    .map(|(name, ms)| duration_row(name.clone(), *ms, "step")),
            );
        } else {
            self.phases.clear();
        }

        // Box3D's stages in the order they run, with the parts of its solver
        // under the solve stage.
        let mut physics_rows = Vec::new();
        if stats.is_open(PHYSICS) {
            self.stages.update(means(AppTimingKind::PhysicsStage));
            self.solve.update(means(AppTimingKind::PhysicsSolve));
            for (name, ms) in self.stages.entries() {
                physics_rows.push(duration_row(name.clone(), *ms, "step"));
                if name == SOLVE_STAGE {
                    physics_rows.extend(
                        self.solve
                            .entries()
                            .iter()
                            .map(|(part, ms)| duration_row(part.clone(), *ms, "step").depth(1)),
                    );
                }
            }
        } else {
            self.stages.clear();
            self.solve.clear();
        }

        vec![
            app.group()
                .headline(step_headline(own))
                .bad(window.late.bad)
                .budget_ms(per_frame)
                .budget_label(APP_BUDGET_LABEL)
                .rows(app_rows),
            physics_group
                .headline(step_headline(box3d))
                .rows(physics_rows),
        ]
    }
}
