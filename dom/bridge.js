// bridge.js — the page's view of the bridge block. Mirrors app/src/bridge.h,
// which is the authoritative description of the layout; read that first.
//
// This is the only file in dom/ that knows byte offsets. Section positions
// come from the block's own section table, so only the fixed header fields
// (and the field order inside each section) are written down here.
//
// The block lives in the app worker's shared WebAssembly memory. The memory
// never grows, so the typed-array views made in the constructor stay valid
// until the app worker is replaced (then make a new Bridge).

export const BRIDGE_MAGIC = 0x42535741; // 'AWSB'
export const BRIDGE_VERSION = 1;

export const RING = Object.freeze({
  DOM_TO_APP: 0,
  RENDERER_TO_APP: 1,
  APP_TO_RENDERER: 2,
  APP_TO_DOM: 3,
});

// bridge.h BRIDGE_KEY_*: what the page reports. What a key does is up to the app.
export const KEY = Object.freeze({
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
});
const KEY_COUNT = 64;

// bridge.h record types this file uses.
export const RECORD = Object.freeze({
  EV_KEY: 1,
  EV_POINTER_DOWN: 2,
  EV_POINTER_UP: 3,
  EV_POINTER_MOVE: 4,
  EV_WHEEL: 5,
  CUE: 48,
});

// What kind of device a pointer event came from (BRIDGE_POINTER_* in bridge.h).
export const POINTER = Object.freeze({
  MOUSE: 0,
  TOUCH: 1,
  PEN: 2,
});

// An event timeStamp (milliseconds, with a fraction) as the bridge carries it:
// whole microseconds, wrapped to 32 bits.
function toMicroseconds(timeMs) {
  return Math.round(timeMs * 1000) % 0x100000000;
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

// ── Byte offsets of the fixed fields (see the comments in bridge.h) ────────

const HEADER = {
  magic: 0,
  version: 4,
  totalSize: 8,
  headerSize: 12,
  stepHz: 16,
  frameTick: 20,
  latestStep: 24,
  inputOffset: 32,
  statsOffset: 40,
  ringOffset: 72, // [4]
  ringHeaderSize: 88,
  recordSize: 92,
  minimumSize: 96, // the header must reach at least past recordSize
};
const INPUT = {
  keys: 0, // [2]
  canvasWidth: 8,
  canvasHeight: 12,
  pointerNdcX: 16,
  pointerNdcY: 20,
  pointerButtons: 24,
};
const STATS = {
  stepWorkUs: 0,
  activePoses: 4,
  frameCount: 8,
  frameWorkUs: 12,
  gpuFrameUs: 16,
  gpuFrameCount: 20,
  syncErrorMax: 24, // f32 bits
  displayLag: 28, // f32 bits
  solverThreads: 32,
};
const RING_FIELDS = {
  head: 0,
  tail: 4,
  capacity: 8,
  dropped: 12,
};
const RECORD_FIELDS = {
  type: 0,
  step: 4,
  payload: 8, // 14 words, read as u32 or f32
};
const PAYLOAD_WORDS = 14;

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
    return this.#u32[this.#word];
  }

  get step() {
    return this.#u32[this.#word + 1];
  }

  // Payload word `i` as an unsigned integer (the C side's `u[i]`).
  u(i) {
    return this.#u32[this.#word + 2 + i];
  }

  // Payload word `i` as a float (the C side's `f[i]`).
  f(i) {
    return this.#f32[this.#word + 2 + i];
  }
}

export class Bridge {
  #u32;
  #f32;
  #base; // block start, in bytes
  #input;
  #stats;
  #rings;
  #recordSize;
  #ringHeaderSize;
  #record;
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
    if (!Number.isInteger(ptr) || ptr <= 0 || ptr % 4 !== 0 || ptr + HEADER.minimumSize > memory.byteLength) {
      throw new Error(`bridge: bad block address ${ptr}`);
    }
    this.#u32 = new Uint32Array(memory);
    this.#f32 = new Float32Array(memory);
    this.#base = ptr;

    const magic = this.#header(HEADER.magic);
    const version = this.#header(HEADER.version);
    if (magic !== BRIDGE_MAGIC) {
      throw new Error(`bridge: no bridge block at ${ptr} (magic 0x${magic.toString(16)})`);
    }
    if (version !== BRIDGE_VERSION) {
      throw new Error(
        `bridge: version mismatch: the app speaks v${version}, this page speaks v${BRIDGE_VERSION}. ` +
          'Rebuild everything (task build).',
      );
    }
    const totalSize = this.#header(HEADER.totalSize);
    if (this.#header(HEADER.headerSize) < HEADER.minimumSize || ptr + totalSize > memory.byteLength) {
      throw new Error('bridge: the block header is inconsistent');
    }

    this.#input = ptr + this.#header(HEADER.inputOffset);
    this.#stats = ptr + this.#header(HEADER.statsOffset);
    this.#rings = [0, 1, 2, 3].map((i) => ptr + this.#header(HEADER.ringOffset + 4 * i));
    this.#ringHeaderSize = this.#header(HEADER.ringHeaderSize);
    this.#recordSize = this.#header(HEADER.recordSize);
    if (this.#ringHeaderSize < 16 || this.#recordSize < RECORD_FIELDS.payload + 4 * PAYLOAD_WORDS) {
      throw new Error('bridge: the ring layout is smaller than this page expects');
    }
    this.#record = new RecordView(this.#u32, this.#f32);
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

  #bitsFloat(bits) {
    this.#bitsU32[0] = bits;
    return this.#bitsF32[0];
  }

  // ── Input state (continuous; the app polls it every step) ──────────────

  setKeyHeld(key, held) {
    if (!(key > 0 && key < KEY_COUNT)) return;
    const word = Bridge.#word(this.#input + INPUT.keys) + (key >>> 5);
    const bit = 1 << (key & 31);
    if (held) {
      Atomics.or(this.#u32, word, bit);
    } else {
      Atomics.and(this.#u32, word, ~bit);
    }
  }

  setCanvasSize(width, height) {
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.canvasWidth), width >>> 0);
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.canvasHeight), height >>> 0);
  }

  // Floats go in as their bit patterns, read back in C by bridge_pointer_ndc_x/y.
  setPointer(ndcX, ndcY, buttons) {
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.pointerNdcX), this.#floatBits(ndcX));
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.pointerNdcY), this.#floatBits(ndcY));
    Atomics.store(this.#u32, Bridge.#word(this.#input + INPUT.pointerButtons), buttons >>> 0);
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

  // Producer side of the dom → app ring. `fill(u, f)` writes the payload
  // through two views of the same 14 words (u = Uint32Array, f = Float32Array).
  #push(type, fill) {
    const ring = this.#rings[RING.DOM_TO_APP];
    const headIndex = Bridge.#word(ring + RING_FIELDS.head);
    const head = Atomics.load(this.#u32, headIndex); // only this thread writes it
    const tail = Atomics.load(this.#u32, Bridge.#word(ring + RING_FIELDS.tail));
    const capacity = this.#u32[Bridge.#word(ring + RING_FIELDS.capacity)];
    if (((head - tail) >>> 0) >= capacity) {
      Atomics.add(this.#u32, Bridge.#word(ring + RING_FIELDS.dropped), 1);
      return false;
    }

    const record = ring + this.#ringHeaderSize + (head & (capacity - 1)) * this.#recordSize;
    const word = Bridge.#word(record);
    this.#u32[word] = type;
    this.#u32[word + 1] = 0; // the page has no step number
    const payload = word + (RECORD_FIELDS.payload >>> 2);
    this.#u32.fill(0, payload, payload + PAYLOAD_WORDS);
    fill(
      this.#u32.subarray(payload, payload + PAYLOAD_WORDS),
      this.#f32.subarray(payload, payload + PAYLOAD_WORDS),
    );

    // Publish: the record's words are written before the new head is visible.
    Atomics.store(this.#u32, headIndex, (head + 1) >>> 0);
    return true;
  }

  // ── Events (app → dom ring) ─────────────────────────────────────────────

  // Consumer side: calls `onRecord(view)` for every waiting record, oldest
  // first. `view` has .type, .step, .u(i) and .f(i), and is reused.
  drain(onRecord) {
    const ring = this.#rings[RING.APP_TO_DOM];
    const tailIndex = Bridge.#word(ring + RING_FIELDS.tail);
    let tail = Atomics.load(this.#u32, tailIndex); // only this thread writes it
    const head = Atomics.load(this.#u32, Bridge.#word(ring + RING_FIELDS.head));
    const capacity = this.#u32[Bridge.#word(ring + RING_FIELDS.capacity)];
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

  // Counters for the stats panel. The counters (…Us, …Count, latestStep)
  // wrap at 2^32; subtract with `(b - a) >>> 0`. displayLag is in steps.
  // solverThreads is 0 from an app that predates the field.
  readStats() {
    const s = this.#stats;
    const load = (byteOffset) => Atomics.load(this.#u32, Bridge.#word(s + byteOffset));
    return {
      stepWorkUs: load(STATS.stepWorkUs),
      activePoses: load(STATS.activePoses),
      frameCount: load(STATS.frameCount),
      frameWorkUs: load(STATS.frameWorkUs),
      gpuFrameUs: load(STATS.gpuFrameUs),
      gpuFrameCount: load(STATS.gpuFrameCount),
      displayLag: this.#bitsFloat(load(STATS.displayLag)),
      solverThreads: load(STATS.solverThreads),
      latestStep: Atomics.load(this.#u32, Bridge.#word(this.#base + HEADER.latestStep)),
    };
  }

  // The largest display-cursor error (in steps) the renderer has seen since
  // the previous call. Reading resets it: the renderer keeps a running
  // maximum, and this puts 0 back in the same atomic step.
  takeSyncError() {
    const word = Bridge.#word(this.#stats + STATS.syncErrorMax);
    return this.#bitsFloat(Atomics.exchange(this.#u32, word, 0));
  }
}
