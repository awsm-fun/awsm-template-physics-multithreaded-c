// bridge.c — the app's side of the bridge block (see bridge.h for the layout).
//
// This file is the only C code that knows how the block is laid out. Game code
// uses the functions declared at the bottom of bridge.h and never computes an
// offset itself.

#include "bridge.h"

#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/atomic.h>
#endif

static_assert((BRIDGE_ALIGN & (BRIDGE_ALIGN - 1u)) == 0, "BRIDGE_ALIGN must be a power of two");

static uint32_t align_up(uint32_t n) {
    return (n + (BRIDGE_ALIGN - 1u)) & ~(BRIDGE_ALIGN - 1u);
}

// A section's address from its byte offset in the table.
static void *at(bridge_header *bridge, uint32_t offset) {
    return (unsigned char *)bridge + offset;
}

// Fills the section table of `h` and returns the total block size. Every
// section is rounded up to BRIDGE_ALIGN so each starts on its own cache line:
// the app writing one section never slows down a reader of the next.
static uint32_t fill_layout(bridge_header *h) {
    uint32_t offset = align_up((uint32_t)sizeof(bridge_header));

    h->header_size = (uint32_t)sizeof(bridge_header);

    h->input_offset = offset;
    h->input_size = (uint32_t)sizeof(bridge_input);
    offset += align_up(h->input_size);

    h->stats_offset = offset;
    h->stats_size = (uint32_t)sizeof(bridge_stats);
    offset += align_up(h->stats_size);

    h->snapshot_offset = offset;
    h->snapshot_count = BRIDGE_SNAPSHOT_COUNT;
    h->snapshot_header_size = (uint32_t)sizeof(bridge_snapshot_header);
    h->pose_capacity = BRIDGE_POSE_CAPACITY;
    h->pose_stride = (uint32_t)sizeof(bridge_pose);
    h->snapshot_stride = align_up(h->snapshot_header_size + h->pose_capacity * h->pose_stride);
    offset += h->snapshot_count * h->snapshot_stride;

    h->ring_header_size = (uint32_t)sizeof(bridge_ring);
    h->record_size = (uint32_t)sizeof(bridge_record);
    uint32_t ring_bytes = align_up(h->ring_header_size + BRIDGE_RING_CAPACITY * h->record_size);
    for (int i = 0; i < BRIDGE_RING_COUNT; ++i) {
        h->ring_offset[i] = offset;
        offset += ring_bytes;
    }

    h->total_size = offset;
    return offset;
}

// ── The block ──────────────────────────────────────────────────────────────

size_t bridge_block_size(void) {
    bridge_header scratch = {0};
    return fill_layout(&scratch);
}

bridge_header *bridge_init(void *memory, size_t size, float step_hz) {
    size_t needed = bridge_block_size();
    if (memory == nullptr || size < needed || (uintptr_t)memory % BRIDGE_ALIGN != 0) {
        return nullptr;
    }
    // Zero first: every counter, flag and seqlock starts at 0, and no reader
    // can mistake leftover heap bytes for data.
    memset(memory, 0, needed);

    bridge_header *h = memory;
    fill_layout(h);
    h->step_hz = step_hz;
    for (int i = 0; i < BRIDGE_RING_COUNT; ++i) {
        bridge_get_ring(h, i)->capacity = BRIDGE_RING_CAPACITY;
    }

    // The handshake fields go in last, so a block with a valid magic is always
    // a fully initialised one.
    h->version = BRIDGE_VERSION;
    atomic_thread_fence(memory_order_release);
    h->magic = BRIDGE_MAGIC;
    return h;
}

bridge_header *bridge_create(float step_hz) {
    size_t size =
        bridge_block_size(); // already a multiple of BRIDGE_ALIGN, as aligned_alloc requires
    void *memory = aligned_alloc(BRIDGE_ALIGN, size);
    bridge_header *bridge = bridge_init(memory, size, step_hz);
    if (bridge == nullptr) {
        free(memory);
    }
    return bridge;
}

void bridge_destroy(bridge_header *bridge) {
    free(bridge);
}

// ── Sections ───────────────────────────────────────────────────────────────

bridge_input *bridge_get_input(bridge_header *bridge) {
    return at(bridge, bridge->input_offset);
}

bridge_stats *bridge_get_stats(bridge_header *bridge) {
    return at(bridge, bridge->stats_offset);
}

bridge_ring *bridge_get_ring(bridge_header *bridge, int which) {
    return at(bridge, bridge->ring_offset[which]);
}

// ── Event rings ────────────────────────────────────────────────────────────
//
// `head` and `tail` are free-running counters: they only ever increase (and
// wrap at 2^32, which unsigned subtraction handles). `head - tail` is the
// number of records waiting. The producer writes the record THEN publishes it
// by storing the new head (release); the consumer loads head (acquire), so it
// can never see a head that is ahead of the record's bytes. The same pairing
// in the other direction tells the producer when a slot is free again.

// Records start right after the ring header. In this build ring_header_size is
// exactly sizeof(bridge_ring) (see fill_layout), so `ring + 1` is the first.
static bridge_record *ring_records(bridge_ring *ring) {
    return (bridge_record *)(void *)(ring + 1);
}

bool bridge_ring_push(bridge_ring *ring, const bridge_record *record) {
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed); // ours
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head - tail >= ring->capacity) {
        atomic_fetch_add_explicit(&ring->dropped, 1u, memory_order_relaxed);
        return false;
    }
    ring_records(ring)[head & (ring->capacity - 1u)] = *record;
    atomic_store_explicit(&ring->head, head + 1u, memory_order_release);
    return true;
}

bool bridge_ring_pop(bridge_ring *ring, bridge_record *out) {
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed); // ours
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head == tail) {
        return false;
    }
    *out = ring_records(ring)[tail & (ring->capacity - 1u)];
    atomic_store_explicit(&ring->tail, tail + 1u, memory_order_release);
    return true;
}

// ── Snapshots ──────────────────────────────────────────────────────────────
//
// A seqlock lets the renderer copy a snapshot without ever blocking the app:
//   writer: version → odd, write the data, version → even
//   reader: read version, copy, read version again; retry unless both reads
//           were the same even number.
// The app is the only writer, so it never has to wait for anyone.

static bridge_snapshot_header *snapshot_slot(bridge_header *bridge, uint32_t step) {
    uint32_t index = step & (bridge->snapshot_count - 1u);
    return at(bridge, bridge->snapshot_offset + index * bridge->snapshot_stride);
}

bridge_snapshot_header *bridge_snapshot_begin(bridge_header *bridge, uint32_t step) {
    bridge_snapshot_header *snapshot = snapshot_slot(bridge, step);
    uint32_t version = atomic_load_explicit(&snapshot->version, memory_order_relaxed);
    atomic_store_explicit(&snapshot->version, version + 1u, memory_order_relaxed);
    // Keeps the "odd" store from being reordered after the data writes below,
    // so a reader that sees new data also sees that the slot was being written.
    atomic_thread_fence(memory_order_release);
    snapshot->step = step;
    return snapshot;
}

bridge_pose *bridge_snapshot_poses(bridge_snapshot_header *snapshot) {
    // Poses follow the header; snapshot_header_size == sizeof(bridge_snapshot_header).
    return (bridge_pose *)(void *)(snapshot + 1);
}

void bridge_snapshot_publish(bridge_header *bridge, bridge_snapshot_header *snapshot) {
    uint32_t version = atomic_load_explicit(&snapshot->version, memory_order_relaxed);
    atomic_store_explicit(&snapshot->version, version + 1u, memory_order_release);
    atomic_store_explicit(&bridge->latest_step, snapshot->step, memory_order_release);
}

// ── Input state ────────────────────────────────────────────────────────────

bool bridge_key_held(bridge_input *input, uint32_t key) {
    if (key >= BRIDGE_KEY_COUNT) {
        return false;
    }
    uint32_t word = atomic_load_explicit(&input->keys[key / 32u], memory_order_relaxed);
    return ((word >> (key % 32u)) & 1u) != 0;
}

// The page stores floats as their IEEE-754 bit patterns in atomic u32 words.
static float float_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

float bridge_pointer_ndc_x(bridge_input *input) {
    return float_from_bits(atomic_load_explicit(&input->pointer_ndc_x, memory_order_relaxed));
}

float bridge_pointer_ndc_y(bridge_input *input) {
    return float_from_bits(atomic_load_explicit(&input->pointer_ndc_y, memory_order_relaxed));
}

uint32_t bridge_pointer_buttons(bridge_input *input) {
    return atomic_load_explicit(&input->pointer_buttons, memory_order_relaxed);
}

float bridge_canvas_aspect(bridge_input *input) {
    uint32_t width = atomic_load_explicit(&input->canvas_width, memory_order_relaxed);
    uint32_t height = atomic_load_explicit(&input->canvas_height, memory_order_relaxed);
    if (width == 0 || height == 0) {
        return 1.0f;
    }
    return (float)width / (float)height;
}

// ── Frame pacing ───────────────────────────────────────────────────────────

uint32_t bridge_frame_tick(bridge_header *bridge) {
    return atomic_load_explicit(&bridge->frame_tick, memory_order_acquire);
}

bool bridge_frame_time(bridge_header *bridge, uint32_t *time_us) {
    // The renderer stores the time, sets the flag, then bumps the tick (all
    // sequentially consistent), so a reader that has seen the tick sees both.
    if (atomic_load_explicit(&bridge->frame_time_valid, memory_order_acquire) == 0) {
        return false;
    }
    *time_us = atomic_load_explicit(&bridge->frame_time_us, memory_order_acquire);
    return true;
}

uint32_t bridge_wait_frame(bridge_header *bridge, uint32_t last) {
#ifdef __EMSCRIPTEN__
    for (;;) {
        uint32_t now = atomic_load_explicit(&bridge->frame_tick, memory_order_acquire);
        if (now != last) {
            return now;
        }
        // `memory.atomic.wait32`: sleeps only while the word still equals
        // `last`, so a tick that lands between the load above and this call is
        // not missed (the wait returns "not-equal" at once). Wake-ups can also
        // be spurious; the loop re-checks either way.
        emscripten_atomic_wait_u32((void *)&bridge->frame_tick, last,
                                   ATOMICS_WAIT_DURATION_INFINITE);
    }
#else
    (void)last;
    return atomic_load_explicit(&bridge->frame_tick, memory_order_acquire);
#endif
}

void bridge_signal_frame_at(bridge_header *bridge, uint32_t time_us) {
    atomic_store_explicit(&bridge->frame_time_us, time_us, memory_order_release);
    atomic_store_explicit(&bridge->frame_time_valid, 1u, memory_order_release);
    bridge_signal_frame(bridge);
}

void bridge_signal_frame(bridge_header *bridge) {
    atomic_fetch_add_explicit(&bridge->frame_tick, 1u, memory_order_release);
#ifdef __EMSCRIPTEN__
    emscripten_atomic_notify((void *)&bridge->frame_tick, EMSCRIPTEN_NOTIFY_ALL_WAITERS);
#endif
}
