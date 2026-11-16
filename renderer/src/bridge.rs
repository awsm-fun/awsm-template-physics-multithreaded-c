//! The renderer's view of the bridge block — the mirror of `app/src/bridge.h`.
//!
//! The bridge block is a region of the APP module's linear memory. This module
//! is a different wasm instance with its own memory, so it cannot dereference
//! a pointer into the block. What it gets instead is the app memory's
//! `SharedArrayBuffer` plus the block's byte offset inside it, and it reads and
//! writes through JS typed-array views.
//!
//! This file is the only place in the renderer that knows the layout.
//!
//! Two rules from the header shape everything here:
//!
//! * Where a section sits, how big its elements are and how many there are all
//!   come from the SECTION TABLE in the block's header at attach time. The app
//!   can grow or move sections without this crate being rebuilt. Only offsets
//!   INSIDE a fixed struct (the header, a pose, a snapshot header, a ring
//!   header, a record) are constants below.
//! * Fields the header marks `_Atomic` are only touched with `Atomics.*`.
//!
//! Nothing on the per-frame path creates a JS object: every view is built once
//! in [`Bridge::attach`] and reused.

use bytemuck::{Pod, Zeroable};
use js_sys::{Atomics, Int32Array, SharedArrayBuffer, Uint8Array};
use wasm_bindgen::JsValue;

/// `BRIDGE_MAGIC`: the bytes 'AWSB', read as a little-endian word.
pub const MAGIC: u32 = 0x4253_5741;

/// `BRIDGE_VERSION` this renderer understands.
pub const VERSION: u32 = 1;

/// `bridge_record.type` values the renderer produces (`SCENE_*`) or consumes
/// (`CMD_*`).
mod record_type {
    pub const SCENE_BEGIN: u32 = 16;
    pub const SCENE_COLLIDER: u32 = 17;
    pub const SCENE_READY: u32 = 18;
    pub const CMD_BIND: u32 = 32;
    pub const CMD_SPAWN: u32 = 33;
    pub const CMD_DESPAWN: u32 = 34;
}

/// `BRIDGE_SHAPE_*`, carried by `SCENE_COLLIDER` records.
mod shape_code {
    pub const BOX: u32 = 1;
    pub const SPHERE: u32 = 2;
    pub const CAPSULE: u32 = 3;
    pub const CYLINDER: u32 = 4;
    pub const CONE: u32 = 5;
}

/// `BRIDGE_POSE_ACTIVE`: the slot holds a live pose in this snapshot.
const POSE_ACTIVE: u32 = 1 << 0;
/// `BRIDGE_POSE_TELEPORT`: the pose jumped this step; do not interpolate into it.
pub const POSE_TELEPORT: u32 = 1 << 1;

/// `bridge_hash()`: 32-bit FNV-1a of the UTF-8 name. Scene nodes and material
/// variants are named across the bridge by this hash, so it must produce
/// exactly what the C and JS versions produce.
pub fn bridge_hash(name: &str) -> u32 {
    let mut hash: u32 = 2_166_136_261;
    for byte in name.bytes() {
        hash ^= u32::from(byte);
        hash = hash.wrapping_mul(16_777_619);
    }
    hash
}

/// Byte offsets of the `bridge_header` fields this renderer reads.
mod header {
    pub const MAGIC: u32 = 0;
    pub const VERSION: u32 = 4;
    pub const TOTAL_SIZE: u32 = 8;
    pub const HEADER_SIZE: u32 = 12;
    pub const STEP_HZ: u32 = 16;
    pub const FRAME_TICK: u32 = 20;
    pub const LATEST_STEP: u32 = 24;
    pub const STATS_OFFSET: u32 = 40;
    pub const STATS_SIZE: u32 = 44;
    pub const SNAPSHOT_OFFSET: u32 = 48;
    pub const SNAPSHOT_COUNT: u32 = 52;
    pub const SNAPSHOT_STRIDE: u32 = 56;
    pub const SNAPSHOT_HEADER_SIZE: u32 = 60;
    pub const POSE_CAPACITY: u32 = 64;
    pub const POSE_STRIDE: u32 = 68;
    /// First of the four ring offsets; ring `i` is at `RING_OFFSET + 4 * i`.
    pub const RING_OFFSET: u32 = 72;
    pub const RING_HEADER_SIZE: u32 = 88;
    pub const RECORD_SIZE: u32 = 92;

    /// End of the last field above. A header at least this long has
    /// everything the renderer needs; anything after it is newer than this
    /// build and is ignored.
    pub const READ_SIZE: u32 = 96;

    /// Written by the renderer at the start of each frame (bridge.h, "Frame
    /// pacing"). They took the place of `reserved1` words, so every header
    /// has room for them; [`FRAME_TIME_END`] is checked anyway.
    pub const FRAME_TIME_US: u32 = 96;
    pub const FRAME_TIME_VALID: u32 = 100;
    pub const FRAME_TIME_END: u32 = 104;

    /// `BRIDGE_RING_RENDERER_TO_APP` / `BRIDGE_RING_APP_TO_RENDERER`.
    pub const RING_RENDERER_TO_APP: u32 = 1;
    pub const RING_APP_TO_RENDERER: u32 = 2;
}

/// Byte offsets inside `bridge_stats` of the fields the renderer owns.
mod stats {
    pub const FRAME_COUNT: u32 = 8;
    pub const FRAME_WORK_US: u32 = 12;
    pub const GPU_FRAME_US: u32 = 16;
    pub const GPU_FRAME_COUNT: u32 = 20;
    pub const FRAME_SPAN_US: u32 = 36;
    pub const APP_LATE: u32 = 40;
    /// End of the last field above. (The section has been 64 bytes from the
    /// start, so even an app built before the later fields existed has room
    /// for them: they were `reserved` words it never touches.)
    pub const READ_SIZE: u32 = 44;
}

/// Word offsets inside `bridge_snapshot_header` of the fields read in place
/// (without copying the snapshot) to find which steps to draw between.
mod snapshot_word {
    pub const VERSION: u32 = 0;
    pub const STEP: u32 = 1;
    pub const TIME_US: u32 = 3;
}

/// Byte offsets inside `bridge_ring` (the header in front of a ring's records).
mod ring {
    pub const HEAD: u32 = 0;
    pub const TAIL: u32 = 4;
    pub const CAPACITY: u32 = 8;
    /// End of the fields this renderer reads. (`dropped` follows; the renderer
    /// never discards a record, so it never writes it.)
    pub const READ_SIZE: u32 = 12;
}

/// `bridge_camera`.
#[derive(Clone, Copy, Debug, Pod, Zeroable)]
#[repr(C)]
pub struct Camera {
    pub eye: [f32; 3],
    pub target: [f32; 3],
    pub up: [f32; 3],
    pub fov_y: f32,
    pub near: f32,
    pub far: f32,
}

/// `bridge_pose`.
#[derive(Clone, Copy, Debug, Pod, Zeroable)]
#[repr(C)]
pub struct Pose {
    pub position: [f32; 3],
    /// Quaternion x, y, z, w.
    pub rotation: [f32; 4],
    pub flags: u32,
}

/// `bridge_snapshot_header`. Every field is listed so the struct has the C
/// layout, including the ones this code has no reason to read by name
/// (`version` is only ever read atomically, in place).
#[derive(Clone, Copy, Debug, Pod, Zeroable)]
#[repr(C)]
#[allow(dead_code)]
struct SnapshotHeader {
    version: u32,
    step: u32,
    pose_count: u32,
    time_us: u32,
    camera: Camera,
}

/// `bridge_record`: one event in a ring. `payload` is the union the header
/// calls `u` / `f`. Which payload word means what depends on `kind` and is
/// spelled out next to the record types in bridge.h; the constructors and
/// [`Record::command`] below are the only code that knows it.
#[derive(Clone, Copy, Debug, Pod, Zeroable)]
#[repr(C)]
pub struct Record {
    kind: u32,
    step: u32,
    payload: [u32; 14],
}

/// A collider shape and its dimensions, as a `SCENE_COLLIDER` record carries
/// them. Capsules, cylinders and cones stand along the collider's local Y.
#[derive(Clone, Copy, Debug)]
pub enum Shape {
    Box { half_extents: [f32; 3] },
    Sphere { radius: f32 },
    Capsule { half_height: f32, radius: f32 },
    Cylinder { half_height: f32, radius: f32 },
    Cone { half_height: f32, radius: f32 },
}

/// One collider node of the scene, in world space: the content of a
/// `SCENE_COLLIDER` record.
#[derive(Clone, Copy, Debug)]
pub struct Collider {
    /// Hash of the collider node's own name.
    pub name: u32,
    /// Hash of the name of its top-level ancestor (itself, if it has none).
    pub root: u32,
    pub shape: Shape,
    pub translation: [f32; 3],
    /// Quaternion x, y, z, w.
    pub rotation: [f32; 4],
}

/// A decoded app-to-renderer record.
#[derive(Clone, Copy, Debug)]
pub enum Command {
    /// `BRIDGE_CMD_BIND`: drive the scene node named `node` from pose `slot`,
    /// wearing material variant `variant` (0 = keep its material).
    Bind { slot: u32, node: u32, variant: u32 },
    /// `BRIDGE_CMD_SPAWN`: create a copy of the scene node named `node`,
    /// driven from pose `slot`, wearing material variant `variant` (0 = the
    /// node's original material).
    Spawn { slot: u32, node: u32, variant: u32 },
    /// `BRIDGE_CMD_DESPAWN`: undo whatever `Bind` or `Spawn` set up for `slot`.
    Despawn { slot: u32 },
    /// A record type this build does not know. Records are fixed-size and
    /// tagged precisely so that such a record can simply be skipped.
    Unknown { kind: u32 },
}

// The same guards as the `static_assert`s at the end of bridge.h.
const _: () = assert!(size_of::<Camera>() == 48);
const _: () = assert!(size_of::<Pose>() == 32);
const _: () = assert!(size_of::<SnapshotHeader>() == 64);
const _: () = assert!(size_of::<Record>() == 64);

/// A record seen as the sixteen 32-bit words that cross the bridge.
type RecordWords = [u32; size_of::<Record>() / 4];

impl Record {
    fn new(kind: u32) -> Self {
        Self {
            kind,
            ..Self::zeroed()
        }
    }

    /// `BRIDGE_SCENE_BEGIN`: a scene description follows.
    pub fn scene_begin() -> Self {
        Self::new(record_type::SCENE_BEGIN)
    }

    /// `BRIDGE_SCENE_COLLIDER`.
    pub fn scene_collider(collider: &Collider) -> Self {
        let (code, dimensions) = match collider.shape {
            Shape::Box { half_extents } => (shape_code::BOX, half_extents),
            Shape::Sphere { radius } => (shape_code::SPHERE, [radius, 0.0, 0.0]),
            Shape::Capsule {
                half_height,
                radius,
            } => (shape_code::CAPSULE, [half_height, radius, 0.0]),
            Shape::Cylinder {
                half_height,
                radius,
            } => (shape_code::CYLINDER, [half_height, radius, 0.0]),
            Shape::Cone {
                half_height,
                radius,
            } => (shape_code::CONE, [half_height, radius, 0.0]),
        };
        let mut record = Self::new(record_type::SCENE_COLLIDER);
        record.payload[0] = collider.name;
        record.payload[1] = collider.root;
        record.payload[2] = code;
        record.set_floats(3, &dimensions);
        record.set_floats(6, &collider.translation);
        record.set_floats(9, &collider.rotation);
        record
    }

    /// `BRIDGE_SCENE_READY`: the description is complete; `collider_count`
    /// collider records were sent.
    pub fn scene_ready(collider_count: u32) -> Self {
        let mut record = Self::new(record_type::SCENE_READY);
        record.payload[0] = collider_count;
        record
    }

    /// The app step at which the record was written (0 if it carries none).
    pub fn step(&self) -> u32 {
        self.step
    }

    /// Decodes a record from the app-to-renderer ring.
    pub fn command(&self) -> Command {
        let [slot, node, variant, ..] = self.payload;
        match self.kind {
            record_type::CMD_BIND => Command::Bind {
                slot,
                node,
                variant,
            },
            record_type::CMD_SPAWN => Command::Spawn {
                slot,
                node,
                variant,
            },
            record_type::CMD_DESPAWN => Command::Despawn { slot },
            kind => Command::Unknown { kind },
        }
    }

    /// Stores `values` as floats in consecutive payload words from `first`.
    fn set_floats(&mut self, first: usize, values: &[f32]) {
        for (word, value) in self.payload[first..].iter_mut().zip(values) {
            *word = value.to_bits();
        }
    }
}

/// How many times a snapshot copy is retried when the app turns out to be
/// writing that snapshot at the same moment. The renderer reads steps the app
/// finished a while ago, so in practice the first attempt succeeds.
const SEQLOCK_ATTEMPTS: u32 = 4;

/// An attached bridge block.
pub struct Bridge {
    /// The whole block as 32-bit words: element `i` is the word at byte offset
    /// `4 * i` from the start of the block. Every atomic field, every header
    /// field and every ring record is read and written through this one view.
    /// (`Atomics.notify` only accepts an `Int32Array`, hence the signed type;
    /// values are reinterpreted as `u32` on the way in and out.)
    words: Int32Array,
    step_hz: f32,

    // Word indices into `words` of the individual atomic fields.
    frame_tick: u32,
    latest_step: u32,
    frame_count: u32,
    frame_work_us: u32,
    gpu_frame_us: u32,
    gpu_frame_count: u32,
    frame_span_us: u32,
    app_late: u32,
    /// Word indices of `frame_time_us` and `frame_time_valid`.
    frame_time_us: u32,
    frame_time_valid: u32,

    snapshots: SnapshotRing,
    to_app: Ring,
    from_app: Ring,
}

/// Where the snapshot ring is and how a snapshot is laid out.
struct SnapshotRing {
    /// Word index of the first snapshot.
    first_word: u32,
    /// Words from one snapshot to the next.
    stride_words: u32,
    count: u32,
    layout: SnapshotLayout,
    /// One byte view per ring entry, over the app's memory. Created once so
    /// that copying a snapshot is a single `set()` call between two existing
    /// views.
    sources: Vec<Uint8Array>,
    /// The two snapshots a frame interpolates between, copied out of the
    /// shared memory.
    copies: [SnapshotCopy; 2],
}

#[derive(Clone, Copy)]
struct SnapshotLayout {
    header_size: usize,
    pose_stride: usize,
    pose_capacity: u32,
}

/// One event ring, seen from the single side (producer or consumer) that the
/// renderer plays on it.
struct Ring {
    /// Word indices of `head` and `tail`.
    head: u32,
    tail: u32,
    /// Word index of the first record.
    first_record: u32,
    /// Words from one record to the next.
    record_words: u32,
    capacity: u32,
    /// The counter this side owns: `head` on a ring the renderer produces
    /// into, `tail` on a ring it consumes from. Kept here so the shared copy
    /// only ever has to be written, never read back.
    own: u32,
}

/// A private copy of one snapshot, in this module's own memory.
struct SnapshotCopy {
    /// `snapshot_stride` bytes. Never resized, so its address is stable and
    /// `view` stays valid for as long as this module's memory does not grow.
    bytes: Vec<u8>,
    /// A JS view over `bytes`: the destination of the bulk copy.
    view: Uint8Array,
    /// The copied header, decoded once per copy.
    header: SnapshotHeader,
    /// Which step `bytes` holds a verified copy of, if any.
    step: Option<u32>,
}

/// A stable, private copy of one published snapshot.
#[derive(Clone, Copy)]
pub struct Snapshot<'a> {
    header: &'a SnapshotHeader,
    bytes: &'a [u8],
    layout: SnapshotLayout,
}

/// The snapshots around one display position, ready to interpolate.
pub struct Sample<'a> {
    /// The snapshot at or just before the display position. `None` only if
    /// nothing readable has been published.
    pub older: Option<Snapshot<'a>>,
    /// The snapshot one step after `older`, when it has been published.
    pub newer: Option<Snapshot<'a>>,
    /// How far the display position is from `older` towards `newer`, 0..1.
    /// Zero when there is no `newer`.
    pub alpha: f32,
}

impl Bridge {
    /// Validates the block at byte offset `bridge_ptr` of `memory` and builds
    /// every view the renderer will use.
    ///
    /// The error is a message meant for the page: a mismatch here means the
    /// app and the renderer were built against different layouts, and drawing
    /// from memory that is not what we think it is would only show garbage.
    pub fn attach(memory: &SharedArrayBuffer, bridge_ptr: u32) -> Result<Self, String> {
        let buffer: &JsValue = memory.as_ref();
        let memory_size = u64::from(memory.byte_length());
        let block_start = u64::from(bridge_ptr);

        if !bridge_ptr.is_multiple_of(4) {
            return Err(format!(
                "the bridge pointer {bridge_ptr:#x} is not 4-byte aligned"
            ));
        }
        if block_start + u64::from(header::READ_SIZE) > memory_size {
            return Err(format!(
                "the bridge pointer {bridge_ptr:#x} lies outside the app memory ({memory_size} bytes)"
            ));
        }

        // Look at the header alone first: nothing else in the block can be
        // trusted until the handshake has passed.
        let head =
            Int32Array::new_with_byte_offset_and_length(buffer, bridge_ptr, header::READ_SIZE / 4);
        let field = |offset: u32| head.get_index(offset / 4) as u32;

        let magic = field(header::MAGIC);
        if magic != MAGIC {
            return Err(format!(
                "no bridge block at {bridge_ptr:#x}: expected magic {MAGIC:#010x}, found {magic:#010x}"
            ));
        }
        let version = field(header::VERSION);
        if version != VERSION {
            return Err(format!(
                "bridge version mismatch: the app speaks version {version}, this renderer speaks \
                 version {VERSION}. Rebuild the renderer against the app's bridge.h."
            ));
        }

        let total_size = field(header::TOTAL_SIZE);
        let header_size = field(header::HEADER_SIZE);
        let total = u64::from(total_size);
        if header_size < header::FRAME_TIME_END
            || total_size < header_size
            || !total_size.is_multiple_of(4)
            || block_start + total > memory_size
        {
            return Err(format!(
                "the bridge header is inconsistent: header_size {header_size}, total_size \
                 {total_size}, at {bridge_ptr:#x} in an app memory of {memory_size} bytes"
            ));
        }

        let step_hz = f32::from_bits(field(header::STEP_HZ));
        if !(1.0..=10_000.0).contains(&step_hz) {
            return Err(format!(
                "the bridge reports an unusable step rate: {step_hz} Hz"
            ));
        }

        // ── stats ──
        let stats_offset = field(header::STATS_OFFSET);
        let stats_size = field(header::STATS_SIZE);
        if stats_size < stats::READ_SIZE {
            return Err(format!(
                "the stats section is too small ({stats_size} bytes)"
            ));
        }
        check_section("stats", stats_offset, u64::from(stats_size), total)?;

        // ── snapshot ring ──
        let snapshot_offset = field(header::SNAPSHOT_OFFSET);
        let snapshot_count = field(header::SNAPSHOT_COUNT);
        let snapshot_stride = field(header::SNAPSHOT_STRIDE);
        let snapshot_header_size = field(header::SNAPSHOT_HEADER_SIZE);
        let pose_capacity = field(header::POSE_CAPACITY);
        let pose_stride = field(header::POSE_STRIDE);
        let poses_end =
            u64::from(snapshot_header_size) + u64::from(pose_capacity) * u64::from(pose_stride);
        if !snapshot_count.is_power_of_two()
            || !snapshot_stride.is_multiple_of(4)
            || (snapshot_header_size as usize) < size_of::<SnapshotHeader>()
            || (pose_stride as usize) < size_of::<Pose>()
            || poses_end > u64::from(snapshot_stride)
        {
            return Err(format!(
                "the snapshot ring layout is inconsistent: {snapshot_count} snapshots of \
                 {snapshot_stride} bytes, a {snapshot_header_size}-byte header, {pose_capacity} \
                 poses of {pose_stride} bytes"
            ));
        }
        check_section(
            "snapshot ring",
            snapshot_offset,
            u64::from(snapshot_count) * u64::from(snapshot_stride),
            total,
        )?;

        // ── event rings ──
        let ring_header_size = field(header::RING_HEADER_SIZE);
        let record_size = field(header::RECORD_SIZE);
        if ring_header_size < ring::READ_SIZE
            || !ring_header_size.is_multiple_of(4)
            || (record_size as usize) < size_of::<Record>()
            || !record_size.is_multiple_of(4)
        {
            return Err(format!(
                "the event ring layout is inconsistent: a {ring_header_size}-byte ring header, \
                 {record_size}-byte records"
            ));
        }

        // The handshake passed and the table is sane: view the whole block.
        let words = Int32Array::new_with_byte_offset_and_length(buffer, bridge_ptr, total_size / 4);

        let open_ring = |which: u32, name: &str| -> Result<Ring, String> {
            let offset = field(header::RING_OFFSET + 4 * which);
            check_section(name, offset, u64::from(ring_header_size), total)?;
            let capacity = words.get_index((offset + ring::CAPACITY) / 4) as u32;
            if !capacity.is_power_of_two() {
                return Err(format!(
                    "the {name} ring has capacity {capacity}, which is not a power of two"
                ));
            }
            let size = u64::from(ring_header_size) + u64::from(capacity) * u64::from(record_size);
            check_section(name, offset, size, total)?;
            Ok(Ring {
                head: (offset + ring::HEAD) / 4,
                tail: (offset + ring::TAIL) / 4,
                first_record: (offset + ring_header_size) / 4,
                record_words: record_size / 4,
                capacity,
                own: 0,
            })
        };
        let mut to_app = open_ring(header::RING_RENDERER_TO_APP, "renderer-to-app")?;
        let mut from_app = open_ring(header::RING_APP_TO_RENDERER, "app-to-renderer")?;
        // Carry on from wherever each ring currently stands.
        to_app.own = load(&words, to_app.head);
        from_app.own = load(&words, from_app.tail);

        let sources = (0..snapshot_count)
            .map(|index| {
                Uint8Array::new_with_byte_offset_and_length(
                    buffer,
                    bridge_ptr + snapshot_offset + index * snapshot_stride,
                    snapshot_stride,
                )
            })
            .collect();
        let snapshots = SnapshotRing {
            first_word: snapshot_offset / 4,
            stride_words: snapshot_stride / 4,
            count: snapshot_count,
            layout: SnapshotLayout {
                header_size: snapshot_header_size as usize,
                pose_stride: pose_stride as usize,
                pose_capacity,
            },
            sources,
            copies: [
                SnapshotCopy::new(snapshot_stride as usize),
                SnapshotCopy::new(snapshot_stride as usize),
            ],
        };

        Ok(Self {
            words,
            step_hz,
            frame_tick: header::FRAME_TICK / 4,
            latest_step: header::LATEST_STEP / 4,
            frame_count: (stats_offset + stats::FRAME_COUNT) / 4,
            frame_work_us: (stats_offset + stats::FRAME_WORK_US) / 4,
            gpu_frame_us: (stats_offset + stats::GPU_FRAME_US) / 4,
            gpu_frame_count: (stats_offset + stats::GPU_FRAME_COUNT) / 4,
            frame_span_us: (stats_offset + stats::FRAME_SPAN_US) / 4,
            app_late: (stats_offset + stats::APP_LATE) / 4,
            frame_time_us: header::FRAME_TIME_US / 4,
            frame_time_valid: header::FRAME_TIME_VALID / 4,
            snapshots,
            to_app,
            from_app,
        })
    }

    /// The app's fixed simulation rate, steps per second.
    pub fn step_hz(&self) -> f32 {
        self.step_hz
    }

    /// Number of pose slots in every snapshot.
    pub fn pose_capacity(&self) -> usize {
        self.snapshots.layout.pose_capacity as usize
    }

    /// Step index of the newest snapshot the app has finished publishing.
    pub fn latest_step(&self) -> u32 {
        load(&self.words, self.latest_step)
    }

    /// Copies out the snapshots to draw for the moment `display_us` on the
    /// frame clock (bridge.h, "Frame pacing"): the two consecutive steps whose
    /// times are around it, and how far between them it lies. Also says
    /// whether the app was late, i.e. had not yet published a step at or past
    /// that moment.
    ///
    /// The newest snapshot is returned on its own (alpha 0) when there is no
    /// pair to interpolate: the app is late (it is held until the app catches
    /// up), `display_us` is `None` (the first frame after attaching), the app
    /// predates step times, or the steps wanted were overwritten after a long
    /// stall.
    pub fn sample_at(&mut self, display_us: Option<u32>) -> (Sample<'_>, bool) {
        let latest = self.latest_step();
        let (older, newer, alpha, late) = match display_us {
            Some(display_us) if latest != 0 => self.bracket(latest, display_us),
            _ => (latest, None, 0.0, false),
        };

        let mut older_copy = self.fetch(older, None);
        let mut newer_copy = match (older_copy, newer) {
            (Some(held), Some(step)) => self.fetch(step, Some(held)),
            _ => None,
        };
        if older_copy.is_none() || (newer.is_some() && newer_copy.is_none()) {
            // The pair could not be read: hold the newest step instead.
            older_copy = self.fetch(latest, None);
            newer_copy = None;
        }

        let snapshots = &self.snapshots;
        (
            Sample {
                older: older_copy.map(|index| snapshots.view(index)),
                newer: newer_copy.map(|index| snapshots.view(index)),
                alpha: if newer_copy.is_some() { alpha } else { 0.0 },
            },
            late,
        )
    }

    /// Finds the steps around `display_us`, reading only the times in the
    /// snapshot headers (no copies). Returns (older step, newer step, alpha,
    /// late).
    fn bracket(&self, latest: u32, display_us: u32) -> (u32, Option<u32>, f32, bool) {
        let hold = (latest, None, 0.0, false);
        let Some(latest_time) = self.step_time(latest) else {
            return hold;
        };
        if latest_time == 0 {
            return hold; // an app from before step times
        }
        // Signed, because the frame clock wraps.
        let after = |time: u32| time.wrapping_sub(display_us) as i32 > 0;
        if !after(latest_time) {
            // The app has not published that far yet.
            return (latest, None, 0.0, true);
        }
        // Walk back to the newest step at or before the moment. While this
        // runs the app may be writing its next burst, which only ever adds
        // steps after `latest`; the ring holds enough that the steps wanted
        // here are not overwritten (fixed_step.c).
        let mut newer = latest;
        let mut newer_time = latest_time;
        for _ in 1..self.snapshots.count {
            let step = newer.wrapping_sub(1);
            let Some(time) = self.step_time(step) else {
                break;
            };
            if !after(time) {
                let span = newer_time.wrapping_sub(time) as f32;
                let into = display_us.wrapping_sub(time) as f32;
                let alpha = if span > 0.0 {
                    (into / span).clamp(0.0, 1.0)
                } else {
                    0.0
                };
                return (step, Some(newer), alpha, false);
            }
            newer = step;
            newer_time = time;
        }
        hold
    }

    /// The `time_us` of the snapshot holding `step`, read in place under its
    /// seqlock. `None` if that entry is being written or holds another step.
    fn step_time(&self, step: u32) -> Option<u32> {
        let ring = &self.snapshots;
        let first = ring.first_word + (step & (ring.count - 1)) * ring.stride_words;
        let before = load(&self.words, first + snapshot_word::VERSION);
        if before & 1 != 0 {
            return None;
        }
        let held = load(&self.words, first + snapshot_word::STEP);
        let time = load(&self.words, first + snapshot_word::TIME_US);
        if load(&self.words, first + snapshot_word::VERSION) != before || held != step {
            return None;
        }
        Some(time)
    }

    /// Makes sure one of the two private buffers holds a verified copy of
    /// `step` and returns which. `keep` names a buffer that must not be
    /// overwritten (it holds the other half of the pair being assembled).
    ///
    /// A published step never changes, so a buffer that already holds `step`
    /// is reused as is: when the display advances by less than one step per
    /// frame, most frames copy nothing at all.
    fn fetch(&mut self, step: u32, keep: Option<usize>) -> Option<usize> {
        let ring = &mut self.snapshots;
        if let Some(found) = ring.copies.iter().position(|copy| copy.step == Some(step)) {
            return Some(found);
        }

        let target = if keep == Some(0) { 1 } else { 0 };
        // Step N lives in ring entry `N & (count - 1)`.
        let entry = step & (ring.count - 1);
        let version = ring.first_word + entry * ring.stride_words;
        let source = &ring.sources[entry as usize];
        let copy = &mut ring.copies[target];
        copy.step = None;

        // Seqlock read: the copy only counts if the snapshot's version word
        // was even (not being written) before it and unchanged after it.
        for _ in 0..SEQLOCK_ATTEMPTS {
            let before = load(&self.words, version);
            if before & 1 != 0 {
                continue;
            }
            copy.fill_from(source);
            if load(&self.words, version) != before {
                continue;
            }
            // A clean copy, but the ring overwrites in place: this entry may
            // hold a different step by now (or not hold `step` yet).
            if copy.header.step != step {
                return None;
            }
            copy.step = Some(step);
            return Some(target);
        }
        None
    }

    /// Queues `record` for the app. Returns `false`, leaving the ring
    /// untouched, when the ring is full; the caller keeps the record and tries
    /// again next frame, so nothing is ever dropped on this ring.
    pub fn try_send(&mut self, record: &Record) -> bool {
        let ring = &mut self.to_app;
        let tail = load(&self.words, ring.tail);
        if ring.own.wrapping_sub(tail) >= ring.capacity {
            return false;
        }
        let first = ring.first_record + (ring.own & (ring.capacity - 1)) * ring.record_words;
        let words: &RecordWords = bytemuck::cast_ref(record);
        for (offset, word) in (0u32..).zip(words) {
            self.words.set_index(first + offset, *word as i32);
        }
        // Publishing the new head is what hands the record over, so it comes
        // last and is atomic.
        ring.own = ring.own.wrapping_add(1);
        store(&self.words, ring.head, ring.own);
        true
    }

    /// Takes the next command the app has queued, if there is one.
    pub fn receive(&mut self) -> Option<Record> {
        let ring = &mut self.from_app;
        let head = load(&self.words, ring.head);
        let pending = head.wrapping_sub(ring.own);
        if pending == 0 {
            return None;
        }
        if pending > ring.capacity {
            // More records "pending" than the ring can hold: the counters are
            // not what a well-behaved producer writes. Skip to the producer's
            // position rather than replaying stale slots as commands.
            tracing::warn!("the app-to-renderer ring is inconsistent; discarding its contents");
            ring.own = head;
            store(&self.words, ring.tail, ring.own);
            return None;
        }

        let first = ring.first_record + (ring.own & (ring.capacity - 1)) * ring.record_words;
        let mut words: RecordWords = [0; size_of::<Record>() / 4];
        for (offset, word) in (0u32..).zip(&mut words) {
            *word = self.words.get_index(first + offset) as u32;
        }
        // Advancing the tail frees the slot for the producer, so the record is
        // read out first.
        ring.own = ring.own.wrapping_add(1);
        store(&self.words, ring.tail, ring.own);
        Some(bytemuck::cast(words))
    }

    /// Starts a frame for the app: stores the frame's time on the frame clock,
    /// marks it valid, then bumps the frame tick and wakes the app, which
    /// sleeps on that word between frames. Done at the START of the frame, so
    /// the app's burst has the whole frame to run in.
    pub fn begin_frame(&self, frame_time_us: u32) {
        store(&self.words, self.frame_time_us, frame_time_us);
        store(&self.words, self.frame_time_valid, 1);
        let _ = Atomics::add(&self.words, self.frame_tick, 1);
        let _ = Atomics::notify(&self.words, self.frame_tick);
    }

    /// Updates the renderer's stats once a frame is drawn. `span_us` is the
    /// time since the previous frame by the browser's frame timestamps (0 for
    /// the first); it is added before the frame is counted, so a page that
    /// sees the new count also sees it. `late`: the frame held the newest
    /// step because the app had not published the moment it wanted.
    pub fn frame_presented(&self, work_us: u32, span_us: u32, late: bool) {
        if late {
            let _ = Atomics::add(&self.words, self.app_late, 1);
        }
        let _ = Atomics::add(&self.words, self.frame_span_us, span_us as i32);
        let _ = Atomics::add(&self.words, self.frame_count, 1);
        let _ = Atomics::add(&self.words, self.frame_work_us, work_us as i32);
    }

    /// Publishes the renderer's GPU frame time for the page's stats panel:
    /// the running total in microseconds and how many frames it sums, both
    /// wrapping. The page divides the change in one by the change in the
    /// other.
    pub fn set_gpu_totals(&self, total_us: u32, frames: u32) {
        store(&self.words, self.gpu_frame_us, total_us);
        store(&self.words, self.gpu_frame_count, frames);
    }
}

impl SnapshotRing {
    fn view(&self, index: usize) -> Snapshot<'_> {
        let copy = &self.copies[index];
        Snapshot {
            header: &copy.header,
            bytes: &copy.bytes,
            layout: self.layout,
        }
    }
}

impl SnapshotCopy {
    fn new(size: usize) -> Self {
        let mut bytes = vec![0u8; size];
        let view = view_over(&mut bytes);
        Self {
            bytes,
            view,
            header: SnapshotHeader::zeroed(),
            step: None,
        }
    }

    /// Bulk-copies one whole snapshot out of the app's memory.
    fn fill_from(&mut self, source: &Uint8Array) {
        // A view over this module's memory goes dead (zero length) when that
        // memory grows, because growing replaces the underlying buffer. That
        // is the only time a new JS object is made on this path.
        if self.view.length() as usize != self.bytes.len() {
            self.view = view_over(&mut self.bytes);
        }
        self.view.set(source.as_ref(), 0);
        self.header = bytemuck::pod_read_unaligned(&self.bytes[..size_of::<SnapshotHeader>()]);
    }
}

/// A JS byte view over a buffer in this module's own memory.
///
/// Takes the `Vec` itself rather than a slice on purpose: `Vec::as_mut_ptr`
/// returns the allocation's pointer without creating a reference to the
/// contents, so reading the buffer through the `Vec` later does not invalidate
/// the pointer the view was built from.
#[allow(clippy::ptr_arg)]
fn view_over(bytes: &mut Vec<u8>) -> Uint8Array {
    // SAFETY: the view aliases the Vec's heap allocation, so two things must
    // hold. The allocation must outlive the view and never move: the Vec is
    // stored next to the view in `SnapshotCopy` and is never resized. And JS
    // must not write through the view while Rust holds a reference into the
    // buffer: the only write is the `set()` call in `fill_from`, during which
    // no such reference exists.
    unsafe { Uint8Array::view_mut_raw(bytes.as_mut_ptr(), bytes.len()) }
}

impl<'a> Snapshot<'a> {
    /// Which step this snapshot holds.
    pub fn step(&self) -> u32 {
        self.header.step
    }

    pub fn camera(&self) -> &'a Camera {
        &self.header.camera
    }

    /// Slots at or beyond this index are not in use in this snapshot.
    pub fn pose_count(&self) -> usize {
        self.header.pose_count.min(self.layout.pose_capacity) as usize
    }

    /// The pose in `slot`, or `None` if the slot is not active in this
    /// snapshot (inactive slots are to be ignored).
    pub fn pose(&self, slot: usize) -> Option<Pose> {
        if slot >= self.pose_count() {
            return None;
        }
        // `attach` checked that every pose of a full snapshot fits inside the
        // snapshot, and the copy is a whole snapshot, so this range exists.
        // The pose is copied out byte-wise, which makes its alignment in the
        // buffer irrelevant.
        let start = self.layout.header_size + slot * self.layout.pose_stride;
        let pose: Pose =
            bytemuck::pod_read_unaligned(&self.bytes[start..start + size_of::<Pose>()]);
        (pose.flags & POSE_ACTIVE != 0).then_some(pose)
    }
}

/// Checks that a section of `size` bytes at `offset` lies inside the block and
/// starts on a word boundary (it is addressed through a 32-bit view).
fn check_section(name: &str, offset: u32, size: u64, total: u64) -> Result<(), String> {
    if !offset.is_multiple_of(4) {
        return Err(format!(
            "the {name} section starts at {offset}, which is not 4-byte aligned"
        ));
    }
    if u64::from(offset) + size > total {
        return Err(format!(
            "the {name} section ({size} bytes at {offset}) does not fit in the {total}-byte bridge block"
        ));
    }
    Ok(())
}

// `Atomics.*` only throws for an index outside the view or a view of the wrong
// kind. `attach` rules both out, so a failure is treated as "no value" rather
// than surfaced on every call.

fn load(words: &Int32Array, word: u32) -> u32 {
    Atomics::load(words, word).unwrap_or(0) as u32
}

fn store(words: &Int32Array, word: u32, value: u32) {
    let _ = Atomics::store(words, word, value as i32);
}
