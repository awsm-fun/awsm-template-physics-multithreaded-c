//! Pose slots: what each slot of a snapshot moves in the renderer.
//!
//! The app publishes a pose per slot per step. A slot means nothing until the
//! app says what it drives, with one of two commands:
//!
//! * `BIND` — an existing scene node follows the slot.
//! * `SPAWN` — a new copy of a scene node is created and follows the slot.
//!
//! `DESPAWN` undoes either. So does [`Slots::reset`], for every slot at once,
//! which is how the scene gets back to exactly its loaded state when a fresh
//! app instance attaches.
//!
//! # Commands arrive early
//!
//! A command is read the moment the app writes it, but the display runs
//! behind the app: a frame shows a moment from the app's previous burst,
//! while the app is already writing the next (see "Frame pacing" in
//! `app/src/runtime/bridge.h`). Acting on a command right away
//! would show its effect before the displayed time has reached it. So every
//! command takes effect at the app step it is stamped with: a bound or spawned
//! object only follows poses from that step on, and a despawned one stays on
//! screen, at its last pose, until the display gets there. See [`effect_step`].
//!
//! # World poses in a transform hierarchy
//!
//! A pose is a WORLD position and rotation, but a scene node's transform is
//! relative to its parent (the mesh that gets driven is typically a child of
//! a group). There are two ways to reconcile that: convert every pose through
//! the inverse of the parent's world matrix, every frame, or take the node out
//! of the hierarchy for as long as it is driven.
//!
//! This module does the second. `BIND` re-parents the node's transform to the
//! root, where "relative to the parent" and "world" are the same thing, and
//! release gives it back its authored parent and transform. The reasons:
//!
//! * Per frame it is a plain `set_local` with the pose: no matrix inverse and
//!   no matrix decomposition per driven object.
//! * It is exact. Going through a parent that is rotated and non-uniformly
//!   scaled produces shear, which a translation / rotation / scale transform
//!   cannot hold.
//! * A driven object goes where the app says, whatever happens to the group
//!   it was authored under.
//!
//! The same holds for spawned copies, which are created directly under the
//! root.

use awsm_renderer::materials::MaterialKey;
use awsm_renderer::meshes::MeshKey;
use awsm_renderer::transforms::{Transform, TransformKey};
use awsm_renderer::AwsmRenderer;
use glam::{Quat, Vec3};

use crate::bridge::{Sample, Snapshot, POSE_TELEPORT};
use crate::scene::{DrivableNode, SceneIndex};

/// A world position and rotation.
type Placement = (Vec3, Quat);

pub struct Slots {
    /// One entry per pose slot of the attached bridge; `None` while the app
    /// has not said what the slot drives.
    entries: Vec<Option<Slot>>,
    /// Whether any entry may be waiting for its despawn step. Lets the common
    /// frame, with nothing waiting, skip looking for one.
    despawns_pending: bool,
}

struct Slot {
    /// The transform the slot's pose is written to. It sits directly under
    /// the root, so its local transform is its world transform.
    transform: TransformKey,
    /// The authored world scale, kept while position and rotation are driven.
    scale: Vec3,
    /// The first step whose snapshots describe this occupant of the slot.
    since_step: u32,
    /// Set by `DESPAWN`: the first step whose snapshots no longer describe
    /// this occupant. The slot is released once the display reaches it.
    until_step: Option<u32>,
    /// The placement last written, so an object at rest costs nothing.
    written: Option<Placement>,
    target: Target,
}

enum Target {
    /// `BIND`: a node of the loaded scene (an index into the scene's
    /// drivable nodes).
    Node { node: usize, variant_applied: bool },
    /// `SPAWN`: a copy that exists only for this slot.
    Copy {
        /// The copy's transforms: its root first, then one per mesh that sits
        /// at an offset from the root.
        transforms: Vec<TransformKey>,
        meshes: Vec<MeshKey>,
        /// A copy stays hidden until its slot delivers a first pose;
        /// otherwise it would flash at the source node's position.
        shown: bool,
    },
}

/// The app step at which a command takes effect, or 0 for "right away".
///
/// Each command record carries the app step during which it was written, and
/// the snapshot of that step is the first one that reflects it. `BIND` and
/// `SPAWN` use the step as the first one whose poses belong to the slot's new
/// occupant; without that, a slot that is despawned and reused straight away
/// would show the NEW occupant at the OLD occupant's last poses until the
/// display caught up. `DESPAWN` uses it as the step at which the occupant
/// goes.
///
/// A stamp of 0 means the command carries no step. A stamp further ahead than
/// the step the app can be working on right now is not a step to wait for (a
/// spawn would look dead, a despawn would never happen). Both are treated as
/// "right away".
pub fn effect_step(command_step: u32, latest_step: u32) -> u32 {
    if is_at_or_after(latest_step.wrapping_add(1), command_step) {
        command_step
    } else {
        0
    }
}

impl Slots {
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
            despawns_pending: false,
        }
    }

    /// Puts the scene back the way it was loaded — every copy removed, every
    /// bound node back in its authored place with its original material — and
    /// makes room for `capacity` slots.
    pub fn reset(&mut self, renderer: &mut AwsmRenderer, scene: &SceneIndex, capacity: usize) {
        for index in 0..self.entries.len() {
            self.release(renderer, scene, index);
        }
        self.entries.clear();
        self.entries.resize_with(capacity, || None);
        self.despawns_pending = false;
    }

    /// `BRIDGE_CMD_BIND`: the scene node named `node_hash` follows `slot`.
    pub fn bind(
        &mut self,
        renderer: &mut AwsmRenderer,
        scene: &SceneIndex,
        slot: u32,
        node_hash: u32,
        variant_hash: u32,
        since_step: u32,
    ) {
        let Some(node_index) = scene.find(node_hash) else {
            tracing::warn!("BIND: no drivable scene node has the name hash {node_hash:#010x}");
            return;
        };
        let Some(index) = self.claim(renderer, scene, slot, "BIND") else {
            return;
        };
        // A node can only be in one place, so it can only follow one slot.
        let bound_elsewhere = self.entries.iter().position(|entry| {
            matches!(entry, Some(Slot { target: Target::Node { node, .. }, .. }) if *node == node_index)
        });
        if let Some(other) = bound_elsewhere {
            // Moving a node from a slot that is being despawned is ordinary
            // reuse; taking it from a live slot is probably a mistake.
            if !self.is_despawning(other) {
                tracing::warn!(
                    "BIND: the node was bound to slot {other}; it now follows slot {slot}"
                );
            }
            self.release(renderer, scene, other);
        }

        let node = scene.node(node_index);
        // Out of the hierarchy (see the module docs), staying exactly where it
        // was authored until the slot delivers a pose.
        renderer.transforms.set_parent(node.transform, None);
        report(
            renderer
                .transforms
                .set_local(node.transform, node.world.clone()),
            "BIND: moving the node under the root",
        );

        let variant = pick_variant(node, variant_hash, "BIND");
        if let Some(material) = variant {
            for mesh in &node.meshes {
                report(
                    renderer.set_mesh_material(mesh.key, material),
                    "BIND: applying the material variant",
                );
            }
        }

        self.entries[index] = Some(Slot {
            transform: node.transform,
            scale: node.world.scale,
            since_step,
            until_step: None,
            written: None,
            target: Target::Node {
                node: node_index,
                variant_applied: variant.is_some(),
            },
        });
    }

    /// `BRIDGE_CMD_SPAWN`: a new copy of the scene node named `node_hash`
    /// follows `slot`.
    pub fn spawn(
        &mut self,
        renderer: &mut AwsmRenderer,
        scene: &SceneIndex,
        slot: u32,
        node_hash: u32,
        variant_hash: u32,
        since_step: u32,
    ) {
        let Some(node_index) = scene.find(node_hash) else {
            tracing::warn!("SPAWN: no drivable scene node has the name hash {node_hash:#010x}");
            return;
        };
        let Some(index) = self.claim(renderer, scene, slot, "SPAWN") else {
            return;
        };
        let node = scene.node(node_index);
        let variant = pick_variant(node, variant_hash, "SPAWN");

        let root = renderer.transforms.insert(node.world.clone(), None);
        let mut transforms = vec![root];
        let mut meshes = Vec::with_capacity(node.meshes.len());
        for mesh in &node.meshes {
            let parent = match &mesh.offset {
                None => root,
                Some(offset) => {
                    let child = renderer.transforms.insert(offset.clone(), Some(root));
                    transforms.push(child);
                    child
                }
            };
            // A duplicate shares the source's geometry on the GPU; only the
            // transform and the material are its own.
            let Some(copy) = report(
                renderer.duplicate_mesh_with_transform(mesh.key, parent),
                "SPAWN: duplicating a mesh",
            ) else {
                continue;
            };
            // The duplicate starts with whatever the source wears right now,
            // which may be a variant a BIND gave it. Without a variant of its
            // own, a copy gets the material the node was loaded with.
            report(
                renderer.set_mesh_material(copy, variant.unwrap_or(mesh.material)),
                "SPAWN: setting the material",
            );
            report(
                renderer.set_mesh_hidden(copy, true),
                "SPAWN: hiding the copy until its first pose",
            );
            meshes.push(copy);
        }

        if meshes.is_empty() {
            tracing::warn!("SPAWN: nothing could be copied for slot {slot}");
            for transform in transforms.into_iter().rev() {
                renderer.transforms.remove(transform);
            }
            return;
        }

        self.entries[index] = Some(Slot {
            transform: root,
            scale: node.world.scale,
            since_step,
            until_step: None,
            written: None,
            target: Target::Copy {
                transforms,
                meshes,
                shown: false,
            },
        });
    }

    /// `BRIDGE_CMD_DESPAWN`: removes the copy in `slot`, or lets go of the
    /// node bound to it, once the display reaches `at_step` (0: right away).
    ///
    /// Until then the occupant stays on screen and keeps following the poses
    /// from before `at_step`, so it disappears at the same displayed moment as
    /// everything else the app did in that step.
    pub fn despawn(
        &mut self,
        renderer: &mut AwsmRenderer,
        scene: &SceneIndex,
        slot: u32,
        at_step: u32,
    ) {
        let index = slot as usize;
        let Some(entry) = self.entries.get(index) else {
            tracing::warn!(
                "DESPAWN: slot {slot} is out of range (the bridge has {} slots)",
                self.entries.len()
            );
            return;
        };
        let Some(occupant) = entry else {
            tracing::warn!("DESPAWN: slot {slot} is not in use");
            return;
        };
        if occupant.until_step.is_some() {
            tracing::warn!("DESPAWN: slot {slot} is already being despawned");
            return;
        }
        if at_step == 0 {
            self.release(renderer, scene, index);
            return;
        }
        if let Some(occupant) = &mut self.entries[index] {
            occupant.until_step = Some(at_step);
            self.despawns_pending = true;
        }
    }

    /// Moves everything that is driven to where `sample` says it is, and
    /// carries out the despawns the display has now reached.
    pub fn apply(&mut self, renderer: &mut AwsmRenderer, scene: &SceneIndex, sample: &Sample<'_>) {
        if self.despawns_pending {
            self.despawns_pending = false;
            // The display is at or past the older of the two sampled steps.
            let displayed = sample.older.map(|snapshot| snapshot.step());
            for index in 0..self.entries.len() {
                let Some(until) = self.entries[index]
                    .as_ref()
                    .and_then(|slot| slot.until_step)
                else {
                    continue;
                };
                if displayed.is_some_and(|step| is_at_or_after(step, until)) {
                    self.release(renderer, scene, index);
                } else {
                    self.despawns_pending = true;
                }
            }
        }

        let pose_count = |snapshot: Option<Snapshot<'_>>| snapshot.map_or(0, |s| s.pose_count());
        let in_use = pose_count(sample.older).max(pose_count(sample.newer));

        for (index, entry) in self.entries.iter_mut().enumerate().take(in_use) {
            let Some(slot) = entry else {
                continue;
            };
            let older = placement_in(sample.older, index, slot);
            let newer = placement_in(sample.newer, index, slot);
            let placement = match (older, newer) {
                // The normal case: somewhere between two consecutive steps.
                (Some((from, _)), Some((to, false))) => (
                    from.0.lerp(to.0, sample.alpha),
                    from.1.slerp(to.1, sample.alpha),
                ),
                // The next step is a teleport, has no pose for this slot, or
                // is the step the occupant is despawned at: stay at the older
                // pose rather than sweep towards something else.
                (Some((held, _)), _) => held,
                // The slot came alive in the newer step: nothing to come from.
                (None, Some((arrived, _))) => arrived,
                // Not active in either step. Leave things as they are.
                (None, None) => continue,
            };
            if slot.written == Some(placement) {
                continue;
            }

            let transform = Transform {
                translation: placement.0,
                rotation: placement.1,
                scale: slot.scale,
            };
            if let Err(err) = renderer.transforms.set_local(slot.transform, transform) {
                // The transform no longer exists, which only a bug elsewhere
                // could cause. Forget the slot instead of failing every frame.
                tracing::warn!("slot {index} lost its transform ({err}); the slot is dropped");
                *entry = None;
                continue;
            }
            slot.written = Some(placement);

            if let Target::Copy { meshes, shown, .. } = &mut slot.target {
                if !*shown {
                    *shown = true;
                    for &mesh in meshes.iter() {
                        report(
                            renderer.set_mesh_hidden(mesh, false),
                            "showing a spawned copy",
                        );
                    }
                }
            }
        }
    }

    /// Checks that `slot` exists and makes it free, releasing a previous
    /// occupant: the app has evidently reused the slot, and its latest command
    /// is the one to honour.
    fn claim(
        &mut self,
        renderer: &mut AwsmRenderer,
        scene: &SceneIndex,
        slot: u32,
        command: &str,
    ) -> Option<usize> {
        let index = slot as usize;
        match self.entries.get(index) {
            None => {
                tracing::warn!(
                    "{command}: slot {slot} is out of range (the bridge has {} slots)",
                    self.entries.len()
                );
                None
            }
            Some(None) => Some(index),
            Some(Some(occupant)) => {
                // Reusing a slot whose occupant is being despawned is how
                // slots are meant to be recycled: the old occupant goes now,
                // a little early, rather than share the slot. Reusing a live
                // slot is probably a mistake in the app.
                if occupant.until_step.is_none() {
                    tracing::warn!(
                        "{command}: slot {slot} was still in use; its occupant is released"
                    );
                }
                self.release(renderer, scene, index);
                Some(index)
            }
        }
    }

    fn is_despawning(&self, index: usize) -> bool {
        matches!(&self.entries[index], Some(slot) if slot.until_step.is_some())
    }

    /// Empties one slot, undoing everything that filling it did.
    fn release(&mut self, renderer: &mut AwsmRenderer, scene: &SceneIndex, index: usize) {
        let Some(slot) = self.entries[index].take() else {
            return;
        };
        match slot.target {
            Target::Node {
                node,
                variant_applied,
            } => {
                let node = scene.node(node);
                renderer.transforms.set_parent(node.transform, node.parent);
                report(
                    renderer
                        .transforms
                        .set_local(node.transform, node.local.clone()),
                    "restoring a node's authored transform",
                );
                if variant_applied {
                    for mesh in &node.meshes {
                        report(
                            renderer.set_mesh_material(mesh.key, mesh.material),
                            "restoring a node's original material",
                        );
                    }
                }
            }
            Target::Copy {
                transforms, meshes, ..
            } => {
                for mesh in meshes {
                    renderer.remove_mesh(mesh);
                }
                // Meshes first, then the transforms they hung from, children
                // before their root.
                for transform in transforms.into_iter().rev() {
                    renderer.transforms.remove(transform);
                }
            }
        }
    }
}

/// Where one snapshot puts the occupant of slot `index`, and whether the pose
/// is a teleport.
///
/// `None` when there is no snapshot, the snapshot is from before the occupant
/// took the slot or from after it was despawned, the slot is inactive in it,
/// or the pose is not usable (a non-finite position or a zero quaternion would
/// poison the renderer's bounds).
fn placement_in(
    snapshot: Option<Snapshot<'_>>,
    index: usize,
    slot: &Slot,
) -> Option<(Placement, bool)> {
    let snapshot = snapshot?;
    let step = snapshot.step();
    let describes_occupant = is_at_or_after(step, slot.since_step)
        && slot
            .until_step
            .is_none_or(|until| !is_at_or_after(step, until));
    if !describes_occupant {
        return None;
    }
    let pose = snapshot.pose(index)?;
    let position = Vec3::from_array(pose.position);
    let rotation = Quat::from_array(pose.rotation);
    let length_squared = rotation.length_squared();
    if !position.is_finite() || !length_squared.is_finite() || length_squared < 1e-12 {
        return None;
    }
    Some((
        (position, rotation.normalize()),
        pose.flags & POSE_TELEPORT != 0,
    ))
}

/// The material for a variant name hash; `None` for "no variant" (0) and for
/// a hash the node has no variant for.
fn pick_variant(node: &DrivableNode, variant_hash: u32, command: &str) -> Option<MaterialKey> {
    if variant_hash == 0 {
        return None;
    }
    let material = node.variant(variant_hash);
    if material.is_none() {
        tracing::warn!(
            "{command}: the node has no material variant with the name hash {variant_hash:#010x}"
        );
    }
    material
}

/// `step >= since`, for step counters that are allowed to wrap around.
fn is_at_or_after(step: u32, since: u32) -> bool {
    step.wrapping_sub(since) < 1 << 31
}

/// Logs a failed renderer call and carries on. Everything here is a targeted
/// change to one object, so one failure should not stop the rest.
fn report<T, E: std::fmt::Display>(result: Result<T, E>, what: &str) -> Option<T> {
    match result {
        Ok(value) => Some(value),
        Err(err) => {
            tracing::warn!("{what} failed: {err}");
            None
        }
    }
}
