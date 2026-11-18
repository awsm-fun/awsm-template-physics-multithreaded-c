// bridge.js — the page's view of the bridge block. Mirrors app/src/bridge.h,
// which is the authoritative description of the layout; read that first.
//
// This is the only file in dom/ that knows byte offsets, and every one of
// them is in LAYOUT below. Section positions come from the block's own
// section table, so only the fixed header fields (and the field order inside
// each section) are written down here.
//
// The block lives in the app worker's shared WebAssembly memory. The memory
// never grows, so the typed-array views made in the constructor stay valid
// until the app worker is replaced (then make a new Bridge).

// The parts of bridge.h this page uses, by their C names. `task test`
// (tools/check-bridge.mjs) imports this module in Node and compares every
// number here with what the C compiler says (app/tests/bridge_layout.c prints
// it), so this file cannot drift from bridge.h unnoticed. Names compare
// case-insensitively, so `KEY.W` is BRIDGE_KEY_W. Byte offsets unless the
// group says otherwise. The rest of this file only uses numbers from here,
// and nothing in it touches `window` or `document` at the top level, so Node
// can import it.
export const LAYOUT = {
  // BRIDGE_MAGIC ('AWSB' as little-endian bytes) and BRIDGE_VERSION.
  handshake: { magic: 0x42535741, version: 1 },
  // bridge_header
  header: {
    magic: 0,
    version: 4,
    total_size: 8,
    header_size: 12,
    latest_step: 24,
    input_offset: 32,
    stats_offset: 40,
    ring_offset: 72, // [4], indexed by ring_index
    ring_header_size: 88,
    record_size: 92,
    timing_offset: 104,
    timing_capacity: 108,
    timing_stride: 112,
  },
  // bridge_input
  input: {
    keys: 0, // [2], one bit per key
    canvas_width: 8,
    canvas_height: 12,
    pointer_ndc_x: 16,
    pointer_ndc_y: 20,
    pointer_buttons: 24,
  },
  // bridge_stats
  stats: {
    step_work_us: 0,
    active_poses: 4,
    frame_count: 8,
    frame_work_us: 12,
    gpu_frame_us: 16,
    gpu_frame_count: 20,
    solver_threads: 32,
    frame_span_us: 36,
    app_late: 40,
    detail: 44, // written by this page: the `detail` bits below
    physics_step_us: 48,
  },
  // BRIDGE_DETAIL_*: the breakdowns the stats panel is showing.
  detail: { APP: 1, RENDERER: 2, GPU: 4, PHYSICS: 8 },
  // bridge_timing: one named running total of the timing table. `name_size`
  // is the size of its name field.
  timing: { kind: 0, total_ns: 4, count: 8, name: 12, name_size: 52 },
  // BRIDGE_TIMING_*: what an entry measures.
  timing_kind: {
    APP_PHASE: 1,
    RENDERER_PHASE: 2,
    RENDERER_PASS: 3,
    GPU_PASS: 4,
    RENDERER_OVERHEAD: 5,
    PHYSICS_STAGE: 6,
    PHYSICS_SOLVE: 7,
  },
  // bridge_ring
  ring: { head: 0, tail: 4, capacity: 8, dropped: 12 },
  // bridge_record: `payload` is the u[14] / f[14] union.
  record: { type: 0, step: 4, payload: 8 },
  // sizeof(...), in bytes
  size: { ring: 64, record: 64, timing: 64 },
  // BRIDGE_RING_*
  ring_index: { DOM_TO_APP: 0, APP_TO_DOM: 3 },
  // Record types: the dom → app events, and the app → dom cue.
  record_type: {
    EV_KEY: 1,
    EV_POINTER_DOWN: 2,
    EV_POINTER_UP: 3,
    EV_POINTER_MOVE: 4,
    EV_WHEEL: 5,
    EV_FRAME_MARK: 6,
    CUE: 48,
  },
  // BRIDGE_BUTTON_CANCELLED
  button: { CANCELLED: 0xffffffff },
  // BRIDGE_POINTER_*: what kind of device a pointer event came from.
  pointer: { MOUSE: 0, TOUCH: 1, PEN: 2 },
  // BRIDGE_KEY_*: what the page reports. What a key does is up to the app.
  // COUNT is the number of key bits in bridge_input.keys, not a key.
  key: {
    W: 1,
    A: 2,
    S: 3,
    D: 4,
    UP: 5,
    DOWN: 6,
    LEFT: 7,
    RIGHT: 8,
    SPACE: 9,
    SHIFT: 10,
    ENTER: 11,
    ESCAPE: 12,
    R: 13,
    COUNT: 64,
  },
};

export const KEY = LAYOUT.key;
export const RECORD = LAYOUT.record_type;
export const DETAIL = LAYOUT.detail;
export const TIMING = LAYOUT.timing_kind;
export const POINTER = LAYOUT.pointer;
// The button of a press the browser cancelled (a touch that became a scroll).
// No real button has this number, so the app ends the press without taking
// it for a click.
export const BUTTON_CANCELLED = LAYOUT.button.CANCELLED;

const HEADER = LAYOUT.header;
const INPUT = LAYOUT.input;
const STATS = LAYOUT.stats;
const RING = LAYOUT.ring;
const RING_INDEX = LAYOUT.ring_index;
const FIELD = LAYOUT.record;
const TIMING_FIELD = LAYOUT.timing;
const SIZE = LAYOUT.size;
// 32-bit words in a record's payload (14).
const PAYLOAD_WORDS = (SIZE.record - FIELD.payload) / 4;
// The header must reach at least past the last field this page reads.
const HEADER_MIN_SIZE = Math.max(...Object.values(HEADER)) + 4;

// An event timeStamp (milliseconds, with a fraction) as the bridge carries it:
// whole microseconds, wrapped to 32 bits.
function toMicroseconds(timeMs) {
  return Math.round(timeMs * 1000) % 0x100000000;
}

// A requestAnimationFrame timestamp on the frame clock (bridge.h, "Frame
// pacing"): performance.timeOrigin + the timestamp, in whole microseconds,
// wrapped to 32 bits. The renderer and the app use the same clock.
export function frameClockUs(rafMs) {
  return Math.round((performance.timeOrigin + rafMs) * 1000) % 0x100000000;
}

// FNV-1a over the UTF-8 bytes of `name`: the same number bridge_hash() gives in C.
export function bridgeHash(name) {
  let h = 0x811c9dc5;
  for (const byte of new TextEncoder().encode(name)) {
    h ^= byte;
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

// One record taken out of a ring. drain() reuses a single instance, so copy
// out what you need inside the callback.
class RecordView {
  #u32;
  #f32;
  #word = 0;

  constructor(u32, f32) {
    this.#u32 = u32;
    this.#f32 = f32;
  }

  at(byteOffset) {
    this.#word = byteOffset >>> 2;
    return this;
  }

  get type() {
    return this.#u32[this.#word + FIELD.type / 4];
  }

  get step() {
    return this.#u32[this.#word + FIELD.step / 4];
  }

  // Payload word `i` as an unsigned integer (the C side's `u[i]`).
  u(i) {
    return this.#u32[this.#word + FIELD.payload / 4 + i];
  }

  // Payload word `i` as a float (the C side's `f[i]`).
  f(i) {
    return this.#f32[this.#word + FIELD.payload / 4 + i];
  }
}

export class Bridge {
  #u32;
  #f32;
  #base; // block start, in bytes
  #input;
  #stats;
  #toApp; // the dom → app ring
  #fromApp; // the app → dom ring
  #recordSize;
  #ringHeaderSize;
  #record;
  #timings; // the timing table, in bytes
  #timingCapacity; // 0 from an app built before the table
  #timingStride;
  #timingNames = []; // by entry; a name never changes once its kind is set
  // Input has been sent since the last frame mark (see markFrame).
  #unmarked = false;
  // Scratch for turning a float into its bit pattern for an atomic store.
  #bitsF32 = new Float32Array(1);
  #bitsU32 = new Uint32Array(this.#bitsF32.buffer);

  // `memory`: the app's SharedArrayBuffer; `ptr`: byte address of the block.
  // Throws with a readable message if the block is not what this page expects.
  constructor(memory, ptr) {
    if (!(memory instanceof SharedArrayBuffer)) {
      throw new Error('bridge: expected the app memory as a SharedArrayBuffer');
    }
    // Everything in the block is a 32-bit word, read through word-indexed
    // views, so the address has to be a multiple of 4.
    if (!Number.isInteger(ptr) || ptr <= 0 || ptr % 4 !== 0 || ptr + HEADER_MIN_SIZE > memory.byteLength) {
      throw new Error(`bridge: bad block address ${ptr}`);
    }
    this.#u32 = new Uint32Array(memory);
    this.#f32 = new Float32Array(memory);
    this.#base = ptr;

    const magic = this.#header(HEADER.magic);
    const version = this.#header(HEADER.version);
    if (magic !== LAYOUT.handshake.magic) {
      throw new Error(`bridge: no bridge block at ${ptr} (magic 0x${magic.toString(16)})`);
    }
    if (version !== LAYOUT.handshake.version) {
      throw new Error(
        `bridge: version mismatch: the app speaks v${version}, this page speaks ` +
          `v${LAYOUT.handshake.version}. Rebuild everything (task build).`,
      );
    }
    const totalSize = this.#header(HEADER.total_size);
    if (this.#header(HEADER.header_size) < HEADER_MIN_SIZE || ptr + totalSize > memory.byteLength) {
      throw new Error('bridge: the block header is inconsistent');
    }

    this.#input = ptr + this.#header(HEADER.input_offset);
    this.#stats = ptr + this.#header(HEADER.stats_offset);
    const ring = (which) => ptr + this.#header(HEADER.ring_offset + 4 * which);
    this.#toApp = ring(RING_INDEX.DOM_TO_APP);
    this.#fromApp = ring(RING_INDEX.APP_TO_DOM);
    this.#ringHeaderSize = this.#header(HEADER.ring_header_size);
    this.#recordSize = this.#header(HEADER.record_size);
    if (this.#ringHeaderSize < SIZE.ring || this.#recordSize < SIZE.record) {
      throw new Error('bridge: the ring layout is smaller than this page expects');
    }
    this.#record = new RecordView(this.#u32, this.#f32);

    const timings = this.#header(HEADER.timing_offset);
    this.#timings = ptr + timings;
    this.#timingCapacity = this.#header(HEADER.timing_capacity);
    this.#timingStride = this.#header(HEADER.timing_stride);
    if (
      this.#timingCapacity > 0 &&
      (this.#timingStride < SIZE.timing ||
        this.#timings % 4 !== 0 ||
        this.#timingStride % 4 !== 0 ||
        timings + this.#timingCapacity * this.#timingStride > totalSize)
    ) {
      throw new Error('bridge: the timing table is inconsistent');
    }
  }

  #header(byteOffset) {
    return this.#u32[(this.#base + byteOffset) >>> 2];
  }

  // Index into #u32 / #f32 of the 32-bit word at `byteAddress`.
  static #word(byteAddress) {
    return byteAddress >>> 2;
  }

  #floatBits(value) {
    this.#bitsF32[0] = value;
    return this.#bitsU32[0];
  }

  // ── Input state (continuous) ────────────────────────────────────────────
  // The app reads the pointer and the canvas size from here. The held-key
  // bits are kept up to date for any other reader, but the app works out what
  // is held from the key records instead, in order, one page frame at a time.

  setKeyHeld(key, held) {
    if (!(key > 0 && key < KEY.COUNT)) return;
    const word = Bridge.#word(this.#input + INPUT.keys) + (key >>> 5);
    const bit = 1 << (key & 31);
    if (held) {
      Atomics.or(this.#u32, word, bit);
    } else {
      Atomics.and(this.#u32, word, ~bit);
    }
  }

  setCanvasSize(width, height) {
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.canvas_width), width >>> 0);
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.canvas_height), height >>> 0);
  }

  // Floats go in as their bit patterns, read back in C by bridge_pointer_ndc_x/y.
  setPointer(ndcX, ndcY, buttons) {
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.pointer_ndc_x), this.#floatBits(ndcX));
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.pointer_ndc_y), this.#floatBits(ndcY));
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.pointer_buttons), buttons >>> 0);
  }

  // ── Events (dom → app ring) ─────────────────────────────────────────────
  // Each returns false if the ring was full (the app is not keeping up).

  sendKey(key, down) {
    return this.#push(RECORD.EV_KEY, (u) => {
      u[0] = key;
      u[1] = down ? 1 : 0;
    });
  }

  // `kind` is a POINTER.* (mouse, touch or pen). `timeMs` is the event's
  // timeStamp; it crosses as microseconds wrapped to 32 bits, and the app
  // only ever uses differences between two of them.
  sendPointerDown(button, ndcX, ndcY, kind, timeMs) {
    return this.#push(RECORD.EV_POINTER_DOWN, (u, f) => {
      u[0] = button;
      f[1] = ndcX;
      f[2] = ndcY;
      u[3] = kind;
      u[4] = toMicroseconds(timeMs);
    });
  }

  sendPointerUp(button, ndcX, ndcY, kind, timeMs) {
    return this.#push(RECORD.EV_POINTER_UP, (u, f) => {
      u[0] = button;
      f[1] = ndcX;
      f[2] = ndcY;
      u[3] = kind;
      u[4] = toMicroseconds(timeMs);
    });
  }

  sendPointerMove(dxCss, dyCss, ndcX, ndcY, buttons, kind, timeMs) {
    return this.#push(RECORD.EV_POINTER_MOVE, (u, f) => {
      f[0] = dxCss;
      f[1] = dyCss;
      f[2] = ndcX;
      f[3] = ndcY;
      u[4] = buttons;
      u[5] = kind;
      u[6] = toMicroseconds(timeMs);
    });
  }

  sendWheel(dyCss) {
    return this.#push(RECORD.EV_WHEEL, (_u, f) => {
      f[0] = dyCss;
    });
  }

  // The end of this page frame: everything sent before it belongs to this
  // frame or an earlier one. `timeUs` is the frame's time on the frame clock
  // (frameClockUs). The app applies input one page frame at a time by these
  // marks (BRIDGE_EV_FRAME_MARK in bridge.h). Only sent if this frame sent
  // input, so an idle page never fills the ring while the app is paused (an
  // anti-aliasing recompile stops the frames that wake it).
  markFrame(timeUs) {
    if (!this.#unmarked) return true;
    const sent = this.#push(RECORD.EV_FRAME_MARK, (u) => {
      u[0] = timeUs >>> 0;
    });
    if (sent) this.#unmarked = false;
    return sent;
  }

  // Producer side of the dom → app ring. `fill(u, f)` writes the payload
  // through two views of the same 14 words (u = Uint32Array, f = Float32Array).
  #push(type, fill) {
    const ring = this.#toApp;
    const headIndex = Bridge.#word(ring + RING.head);
    const head = Atomics.load(this.#u32, headIndex); // only this thread writes it
    const tail = Atomics.load(this.#u32, Bridge.#word(ring + RING.tail));
    const capacity = this.#u32[Bridge.#word(ring + RING.capacity)];
    if (((head - tail) >>> 0) >= capacity) {
      Atomics.add(this.#u32, Bridge.#word(ring + RING.dropped), 1);
      return false;
    }

    const record = ring + this.#ringHeaderSize + (head & (capacity - 1)) * this.#recordSize;
    const word = Bridge.#word(record);
    this.#u32[word + FIELD.type / 4] = type;
    this.#u32[word + FIELD.step / 4] = 0; // the page has no step number
    const payload = word + FIELD.payload / 4;
    this.#u32.fill(0, payload, payload + PAYLOAD_WORDS);
    fill(
      this.#u32.subarray(payload, payload + PAYLOAD_WORDS),
      this.#f32.subarray(payload, payload + PAYLOAD_WORDS),
    );

    // Publish: the record's words are written before the new head is visible.
    Atomics.store(this.#u32, headIndex, (head + 1) >>> 0);
    if (type !== RECORD.EV_FRAME_MARK) this.#unmarked = true;
    return true;
  }

  // ── Events (app → dom ring) ─────────────────────────────────────────────

  // Consumer side: calls `onRecord(view)` for every waiting record, oldest
  // first. `view` has .type, .step, .u(i) and .f(i), and is reused.
  drain(onRecord) {
    const ring = this.#fromApp;
    const tailIndex = Bridge.#word(ring + RING.tail);
    let tail = Atomics.load(this.#u32, tailIndex); // only this thread writes it
    const head = Atomics.load(this.#u32, Bridge.#word(ring + RING.head));
    const capacity = this.#u32[Bridge.#word(ring + RING.capacity)];
    while (tail !== head) {
      const record = ring + this.#ringHeaderSize + (tail & (capacity - 1)) * this.#recordSize;
      onRecord(this.#record.at(record));
      tail = (tail + 1) >>> 0;
      // Hand the slot back right away, so a slow callback never makes the
      // app see a full ring.
      Atomics.store(this.#u32, tailIndex, tail);
    }
  }

  // ── Reading ─────────────────────────────────────────────────────────────

  // Counters for the stats panel. The counters (…Us, …Count, appLate,
  // latestStep) wrap at 2^32; subtract with `(b - a) >>> 0`. solverThreads is
  // 0 from an app that predates the field.
  readStats() {
    const s = this.#stats;
    const load = (byteOffset) => Atomics.load(this.#u32, Bridge.#word(s + byteOffset));
    return {
      stepWorkUs: load(STATS.step_work_us),
      activePoses: load(STATS.active_poses),
      frameCount: load(STATS.frame_count),
      frameWorkUs: load(STATS.frame_work_us),
      gpuFrameUs: load(STATS.gpu_frame_us),
      gpuFrameCount: load(STATS.gpu_frame_count),
      solverThreads: load(STATS.solver_threads),
      frameSpanUs: load(STATS.frame_span_us),
      appLate: load(STATS.app_late),
      physicsStepUs: load(STATS.physics_step_us),
      latestStep: Atomics.load(this.#u32, Bridge.#word(this.#base + HEADER.latest_step)),
    };
  }

  // Tells the app and the renderer which breakdowns the stats panel is
  // showing (DETAIL bits). They only measure the detail that is asked for.
  setDetail(bits) {
    Atomics.store(this.#u32, (this.#stats + STATS.detail) >>> 2, bits >>> 0);
  }

  // The timing table: every entry in use, as { slot, kind, name, totalNs,
  // count }. `kind` is a TIMING value; `totalNs` and `count` are running
  // totals that wrap at 2^32, and the mean per step or frame over a window is
  // the change in one divided by the change in the other.
  readTimings() {
    const entries = [];
    for (let slot = 0; slot < this.#timingCapacity; slot++) {
      const entry = this.#timings + slot * this.#timingStride;
      const kind = Atomics.load(this.#u32, (entry + TIMING_FIELD.kind) >>> 2);
      if (kind === 0) continue;
      entries.push({
        slot,
        kind,
        name: (this.#timingNames[slot] ??= this.#timingName(entry)),
        totalNs: Atomics.load(this.#u32, (entry + TIMING_FIELD.total_ns) >>> 2),
        count: Atomics.load(this.#u32, (entry + TIMING_FIELD.count) >>> 2),
      });
    }
    return entries;
  }

  // An entry's name: ASCII bytes up to the first zero. (Read byte by byte: a
  // TextDecoder does not accept a view of shared memory.)
  #timingName(entry) {
    const bytes = new Uint8Array(this.#u32.buffer, entry + TIMING_FIELD.name, TIMING_FIELD.name_size);
    let name = '';
    for (const byte of bytes) {
      if (byte === 0) break;
      name += String.fromCharCode(byte);
    }
    return name;
  }
}
