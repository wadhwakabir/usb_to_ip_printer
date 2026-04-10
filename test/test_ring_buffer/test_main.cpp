#include <unity.h>

#include <cstring>

#include "ring_buffer.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint8_t backing_small[8];
static uint8_t backing_medium[64];
static uint8_t backing_large[1024];

static RingBuffer make_buf(uint8_t *storage, size_t capacity) {
  std::memset(storage, 0xCC, capacity);  // poison to catch stale reads
  RingBuffer rb;
  rb.storage = storage;
  rb.capacity = capacity;
  rb.head = 0;
  rb.tail = 0;
  return rb;
}

// Fill `data` with a repeating byte pattern so content is verifiable.
static void fill_pattern(uint8_t *data, size_t len, uint8_t start) {
  for (size_t i = 0; i < len; ++i) {
    data[i] = static_cast<uint8_t>(start + i);
  }
}

// ---------------------------------------------------------------------------
// Basic read / write
// ---------------------------------------------------------------------------

void test_empty_buffer_reports_zero_used() {
  RingBuffer rb = make_buf(backing_small, 8);
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(7u, rb.free_space());  // capacity-1
}

void test_read_from_empty_returns_zero() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t out[4];
  TEST_ASSERT_EQUAL(0u, rb.read(out, sizeof(out)));
}

void test_write_then_read_round_trip() {
  RingBuffer rb = make_buf(backing_small, 8);
  const uint8_t data[] = {0xAA, 0xBB, 0xCC};
  TEST_ASSERT_EQUAL(3u, rb.write(data, 3));
  TEST_ASSERT_EQUAL(3u, rb.used());
  TEST_ASSERT_EQUAL(4u, rb.free_space());

  uint8_t out[4] = {};
  TEST_ASSERT_EQUAL(3u, rb.read(out, sizeof(out)));
  TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xBB, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0xCC, out[2]);
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Full / overflow
// ---------------------------------------------------------------------------

void test_write_fills_to_capacity_minus_one() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable
  uint8_t data[7];
  fill_pattern(data, 7, 0x10);
  TEST_ASSERT_EQUAL(7u, rb.write(data, 7));
  TEST_ASSERT_EQUAL(7u, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());
}

void test_write_to_full_buffer_returns_zero() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[7];
  fill_pattern(data, 7, 0x10);
  rb.write(data, 7);

  uint8_t extra[] = {0xFF};
  TEST_ASSERT_EQUAL(0u, rb.write(extra, 1));
}

void test_write_partial_when_nearly_full() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable
  uint8_t data[5];
  fill_pattern(data, 5, 0x20);
  rb.write(data, 5);  // 5 used, 2 free

  uint8_t more[4];
  fill_pattern(more, 4, 0x30);
  TEST_ASSERT_EQUAL(2u, rb.write(more, 4));  // only 2 fit
  TEST_ASSERT_EQUAL(7u, rb.used());
}

void test_read_partial_when_less_available() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {1, 2, 3};
  rb.write(data, 3);

  uint8_t out[8] = {};
  TEST_ASSERT_EQUAL(3u, rb.read(out, 8));
  TEST_ASSERT_EQUAL_HEX8(1, out[0]);
  TEST_ASSERT_EQUAL_HEX8(2, out[1]);
  TEST_ASSERT_EQUAL_HEX8(3, out[2]);
}

// ---------------------------------------------------------------------------
// Wrap-around
// ---------------------------------------------------------------------------

void test_write_wraps_around() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable

  // Fill 5, drain 5 → tail=5, head=5
  uint8_t tmp[5];
  fill_pattern(tmp, 5, 0x40);
  rb.write(tmp, 5);
  rb.read(tmp, 5);
  TEST_ASSERT_EQUAL(0u, rb.used());

  // Now write 6 bytes — must wrap around the end
  uint8_t data[6];
  fill_pattern(data, 6, 0x50);
  TEST_ASSERT_EQUAL(6u, rb.write(data, 6));
  TEST_ASSERT_EQUAL(6u, rb.used());

  uint8_t out[6] = {};
  TEST_ASSERT_EQUAL(6u, rb.read(out, 6));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 6);
}

void test_read_wraps_around() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance tail to position 6
  uint8_t tmp[6];
  fill_pattern(tmp, 6, 0x60);
  rb.write(tmp, 6);
  rb.read(tmp, 6);

  // Write 5 bytes (wraps head around)
  uint8_t data[5];
  fill_pattern(data, 5, 0x70);
  rb.write(data, 5);

  // Read should reconstruct the original across the wrap
  uint8_t out[5] = {};
  TEST_ASSERT_EQUAL(5u, rb.read(out, 5));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 5);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void test_reset_clears_buffer() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {1, 2, 3, 4};
  rb.write(data, 4);
  rb.reset();
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(7u, rb.free_space());

  // Should be able to write full capacity again
  uint8_t big[7];
  fill_pattern(big, 7, 0x80);
  TEST_ASSERT_EQUAL(7u, rb.write(big, 7));
}

// ---------------------------------------------------------------------------
// Minimum capacity: 2 (1 usable byte due to sentinel)
// ---------------------------------------------------------------------------

void test_capacity_two_single_byte() {
  uint8_t tiny[2];
  RingBuffer rb = make_buf(tiny, 2);
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(1u, rb.free_space());

  uint8_t w = 0xAB;
  TEST_ASSERT_EQUAL(1u, rb.write(&w, 1));
  TEST_ASSERT_EQUAL(1u, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  // Full — second write fails
  uint8_t w2 = 0xCD;
  TEST_ASSERT_EQUAL(0u, rb.write(&w2, 1));

  uint8_t r = 0;
  TEST_ASSERT_EQUAL(1u, rb.read(&r, 1));
  TEST_ASSERT_EQUAL_HEX8(0xAB, r);
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Single-byte operations
// ---------------------------------------------------------------------------

void test_single_byte_write_read_cycle() {
  RingBuffer rb = make_buf(backing_small, 8);

  for (uint8_t i = 0; i < 32; ++i) {
    TEST_ASSERT_EQUAL(1u, rb.write(&i, 1));
    uint8_t out = 0xFF;
    TEST_ASSERT_EQUAL(1u, rb.read(&out, 1));
    TEST_ASSERT_EQUAL_HEX8(i, out);
    TEST_ASSERT_EQUAL(0u, rb.used());
  }
}

// ---------------------------------------------------------------------------
// Streaming: repeated fill-drain cycles
// ---------------------------------------------------------------------------

void test_streaming_fill_drain_cycles() {
  RingBuffer rb = make_buf(backing_medium, 64);  // 63 usable
  const size_t chunk = 20;
  uint8_t wbuf[20];
  uint8_t rbuf[20];

  for (int cycle = 0; cycle < 50; ++cycle) {
    fill_pattern(wbuf, chunk, static_cast<uint8_t>(cycle * 7));
    const size_t written = rb.write(wbuf, chunk);
    TEST_ASSERT_EQUAL(chunk, written);

    std::memset(rbuf, 0, chunk);
    const size_t read = rb.read(rbuf, chunk);
    TEST_ASSERT_EQUAL(chunk, read);
    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, chunk);
  }
}

void test_streaming_partial_drain() {
  // Write large, read small — verify data integrity across many wraps.
  RingBuffer rb = make_buf(backing_large, 1024);  // 1023 usable
  uint8_t pattern[256];
  fill_pattern(pattern, sizeof(pattern), 0);

  size_t total_written = 0;
  size_t total_read = 0;

  for (int i = 0; i < 100; ++i) {
    // Write a burst
    size_t w = rb.write(pattern, sizeof(pattern));
    total_written += w;

    // Drain in small chunks
    uint8_t small[37];
    while (rb.used() > 128) {
      size_t r = rb.read(small, sizeof(small));
      total_read += r;
      if (r == 0) break;
    }
  }

  // Drain remainder
  uint8_t drain[128];
  while (rb.used() > 0) {
    total_read += rb.read(drain, sizeof(drain));
  }

  TEST_ASSERT_EQUAL(total_written, total_read);
}

// ---------------------------------------------------------------------------
// Data integrity across wrap boundary (large)
// ---------------------------------------------------------------------------

void test_wrap_boundary_data_integrity() {
  // Position head/tail near the end of the buffer, then write data that
  // straddles the wrap point and verify every byte.
  RingBuffer rb = make_buf(backing_medium, 64);  // 63 usable

  // Advance to position 60
  uint8_t tmp[60];
  fill_pattern(tmp, 60, 0);
  rb.write(tmp, 60);
  rb.read(tmp, 60);
  // head=60, tail=60

  // Write 10 bytes — 4 fit before wrap, 6 after
  uint8_t data[10];
  fill_pattern(data, 10, 0xA0);
  TEST_ASSERT_EQUAL(10u, rb.write(data, 10));

  uint8_t out[10] = {};
  TEST_ASSERT_EQUAL(10u, rb.read(out, 10));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 10);
}

// ---------------------------------------------------------------------------
// Zero-length operations
// ---------------------------------------------------------------------------

void test_write_zero_length() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {0xFF};
  TEST_ASSERT_EQUAL(0u, rb.write(data, 0));
  TEST_ASSERT_EQUAL(0u, rb.used());
}

void test_read_zero_length() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {1, 2};
  rb.write(data, 2);

  uint8_t out[1];
  TEST_ASSERT_EQUAL(0u, rb.read(out, 0));
  TEST_ASSERT_EQUAL(2u, rb.used());  // unchanged
}

// ---------------------------------------------------------------------------
// used() and free_space() consistency
// ---------------------------------------------------------------------------

void test_used_plus_free_equals_capacity_minus_one() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Check at several fill levels
  for (size_t i = 0; i < 63; ++i) {
    TEST_ASSERT_EQUAL(63u, rb.used() + rb.free_space());
    uint8_t b = static_cast<uint8_t>(i);
    rb.write(&b, 1);
  }
  TEST_ASSERT_EQUAL(63u, rb.used() + rb.free_space());
}

// ---------------------------------------------------------------------------
// Exact capacity fill, drain, refill
// ---------------------------------------------------------------------------

void test_fill_drain_refill() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable
  uint8_t data[7];

  // Fill
  fill_pattern(data, 7, 0x10);
  TEST_ASSERT_EQUAL(7u, rb.write(data, 7));

  // Drain
  uint8_t out[7];
  TEST_ASSERT_EQUAL(7u, rb.read(out, 7));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 7);
  TEST_ASSERT_EQUAL(0u, rb.used());

  // Refill with different data
  fill_pattern(data, 7, 0x90);
  TEST_ASSERT_EQUAL(7u, rb.write(data, 7));

  // Drain again
  TEST_ASSERT_EQUAL(7u, rb.read(out, 7));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 7);
}

// ---------------------------------------------------------------------------
// Interleaved write/read preserves FIFO order
// ---------------------------------------------------------------------------

void test_fifo_order_with_interleaved_ops() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t expected_next = 0;

  for (int i = 0; i < 200; ++i) {
    // Write 3 bytes
    uint8_t w[3];
    for (int j = 0; j < 3; ++j) {
      w[j] = static_cast<uint8_t>((i * 3 + j) & 0xFF);
    }
    size_t written = rb.write(w, 3);
    TEST_ASSERT_EQUAL(3u, written);

    // Read 3 bytes
    uint8_t r[3];
    size_t read_n = rb.read(r, 3);
    TEST_ASSERT_EQUAL(3u, read_n);
    for (int j = 0; j < 3; ++j) {
      TEST_ASSERT_EQUAL_HEX8(expected_next, r[j]);
      expected_next = static_cast<uint8_t>((expected_next + 1) & 0xFF);
    }
  }
}

// ---------------------------------------------------------------------------
// Head at position 0 edge case (write lands exactly at boundary)
// ---------------------------------------------------------------------------

void test_write_lands_exactly_at_end() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance head to position 5, tail to 5
  uint8_t tmp[5];
  rb.write(tmp, 5);
  rb.read(tmp, 5);

  // Write exactly 3 bytes: positions 5,6,7 → head wraps to 0
  uint8_t data[] = {0xD0, 0xD1, 0xD2};
  TEST_ASSERT_EQUAL(3u, rb.write(data, 3));
  // head should be at 0 (wrapped), tail at 5
  TEST_ASSERT_EQUAL(3u, rb.used());

  uint8_t out[3];
  TEST_ASSERT_EQUAL(3u, rb.read(out, 3));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 3);
}

// ---------------------------------------------------------------------------
// Null / degenerate input guards
// ---------------------------------------------------------------------------

void test_write_null_data_returns_zero() {
  RingBuffer rb = make_buf(backing_small, 8);
  TEST_ASSERT_EQUAL(0u, rb.write(nullptr, 5));
  TEST_ASSERT_EQUAL(0u, rb.used());
}

void test_read_null_data_returns_zero() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {1, 2, 3};
  rb.write(data, 3);
  TEST_ASSERT_EQUAL(0u, rb.read(nullptr, 3));
  TEST_ASSERT_EQUAL(3u, rb.used());  // data still in buffer
}

void test_null_storage_write_returns_zero() {
  RingBuffer rb;
  rb.storage = nullptr;
  rb.capacity = 64;
  rb.head = 0;
  rb.tail = 0;
  uint8_t data[] = {0xAA};
  TEST_ASSERT_EQUAL(0u, rb.write(data, 1));
}

void test_null_storage_read_returns_zero() {
  RingBuffer rb;
  rb.storage = nullptr;
  rb.capacity = 64;
  rb.head = 0;
  rb.tail = 0;
  uint8_t out[4];
  TEST_ASSERT_EQUAL(0u, rb.read(out, sizeof(out)));
}

void test_capacity_zero_is_safe() {
  RingBuffer rb;
  rb.storage = backing_small;
  rb.capacity = 0;
  rb.head = 0;
  rb.tail = 0;

  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  uint8_t data[] = {0xFF};
  TEST_ASSERT_EQUAL(0u, rb.write(data, 1));

  uint8_t out;
  TEST_ASSERT_EQUAL(0u, rb.read(&out, 1));
}

void test_capacity_one_is_safe() {
  uint8_t tiny[1];
  RingBuffer rb;
  rb.storage = tiny;
  rb.capacity = 1;
  rb.head = 0;
  rb.tail = 0;

  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  uint8_t data[] = {0xAB};
  TEST_ASSERT_EQUAL(0u, rb.write(data, 1));

  uint8_t out;
  TEST_ASSERT_EQUAL(0u, rb.read(&out, 1));
}

// ---------------------------------------------------------------------------
// Read lands exactly at buffer end (mirror of write test)
// ---------------------------------------------------------------------------

void test_read_lands_exactly_at_end() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance head and tail to position 3
  uint8_t tmp[3];
  rb.write(tmp, 3);
  rb.read(tmp, 3);

  // Write 5 bytes at positions 3,4,5,6,7 (wraps head to 0)
  uint8_t data[] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4};
  TEST_ASSERT_EQUAL(5u, rb.write(data, 5));

  // Read exactly 5 → tail goes from 3 past 7 back to 0
  uint8_t out[5];
  TEST_ASSERT_EQUAL(5u, rb.read(out, 5));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 5);
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Multiple consecutive resets
// ---------------------------------------------------------------------------

void test_multiple_resets_are_safe() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {1, 2, 3};
  rb.write(data, 3);
  rb.reset();
  rb.reset();
  rb.reset();

  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(7u, rb.free_space());

  // Buffer still usable after triple reset
  TEST_ASSERT_EQUAL(3u, rb.write(data, 3));
  uint8_t out[3];
  TEST_ASSERT_EQUAL(3u, rb.read(out, 3));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 3);
}

void test_reset_after_wrap() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance past the wrap point
  uint8_t tmp[6];
  fill_pattern(tmp, 6, 0);
  rb.write(tmp, 6);
  rb.read(tmp, 6);
  fill_pattern(tmp, 5, 0x10);
  rb.write(tmp, 5);  // wraps around

  rb.reset();
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(7u, rb.free_space());

  // Full fill after reset should work cleanly
  uint8_t data[7];
  fill_pattern(data, 7, 0xF0);
  TEST_ASSERT_EQUAL(7u, rb.write(data, 7));
  uint8_t out[7];
  TEST_ASSERT_EQUAL(7u, rb.read(out, 7));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 7);
}

// ---------------------------------------------------------------------------
// Large single write exceeding capacity
// ---------------------------------------------------------------------------

void test_write_larger_than_capacity_writes_partial() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable
  uint8_t data[32];
  fill_pattern(data, 32, 0xB0);

  TEST_ASSERT_EQUAL(7u, rb.write(data, 32));
  TEST_ASSERT_EQUAL(7u, rb.used());

  uint8_t out[7];
  TEST_ASSERT_EQUAL(7u, rb.read(out, 7));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 7);  // first 7 bytes of data
}

// ---------------------------------------------------------------------------
// Fill to near-full, read 1 / write 1 cycle (high-water stress)
// ---------------------------------------------------------------------------

void test_near_full_single_byte_cycling() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable

  // Fill to 6 out of 7
  uint8_t bulk[6];
  fill_pattern(bulk, 6, 0);
  rb.write(bulk, 6);
  TEST_ASSERT_EQUAL(6u, rb.used());
  TEST_ASSERT_EQUAL(1u, rb.free_space());

  // Cycle single bytes at near-full for many iterations
  for (uint8_t i = 0; i < 100; ++i) {
    uint8_t out;
    TEST_ASSERT_EQUAL(1u, rb.read(&out, 1));
    TEST_ASSERT_EQUAL(5u, rb.used());
    TEST_ASSERT_EQUAL(2u, rb.free_space());

    uint8_t w = i;
    TEST_ASSERT_EQUAL(1u, rb.write(&w, 1));
    TEST_ASSERT_EQUAL(6u, rb.used());
    TEST_ASSERT_EQUAL(1u, rb.free_space());
  }
}

// ---------------------------------------------------------------------------
// Full fill at every starting position (exercises all wrap offsets)
// ---------------------------------------------------------------------------

void test_full_fill_at_every_start_position() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable

  for (size_t start = 0; start < 8; ++start) {
    rb.reset();
    // Advance head and tail to `start`
    if (start > 0) {
      uint8_t tmp[7];
      // Can only write up to 7 at a time
      size_t advance = start;
      fill_pattern(tmp, advance, 0);
      rb.write(tmp, advance);
      rb.read(tmp, advance);
    }

    // Now fill completely and verify
    uint8_t data[7];
    fill_pattern(data, 7, static_cast<uint8_t>(start * 10));
    TEST_ASSERT_EQUAL(7u, rb.write(data, 7));
    TEST_ASSERT_EQUAL(7u, rb.used());
    TEST_ASSERT_EQUAL(0u, rb.free_space());

    // Extra write must fail
    uint8_t extra = 0xFF;
    TEST_ASSERT_EQUAL(0u, rb.write(&extra, 1));

    uint8_t out[7];
    TEST_ASSERT_EQUAL(7u, rb.read(out, 7));
    TEST_ASSERT_EQUAL_MEMORY(data, out, 7);
    TEST_ASSERT_EQUAL(0u, rb.used());
  }
}

// ---------------------------------------------------------------------------
// Non-power-of-2 capacities
// ---------------------------------------------------------------------------

static uint8_t backing_odd[13];

void test_non_power_of_two_capacity() {
  RingBuffer rb = make_buf(backing_odd, 13);  // 12 usable
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(12u, rb.free_space());

  uint8_t data[12];
  fill_pattern(data, 12, 0xC0);
  TEST_ASSERT_EQUAL(12u, rb.write(data, 12));
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  uint8_t out[12];
  TEST_ASSERT_EQUAL(12u, rb.read(out, 12));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 12);

  // Streaming across wrap with odd capacity
  for (int cycle = 0; cycle < 50; ++cycle) {
    uint8_t w[7];
    fill_pattern(w, 7, static_cast<uint8_t>(cycle));
    TEST_ASSERT_EQUAL(7u, rb.write(w, 7));
    uint8_t r[7];
    TEST_ASSERT_EQUAL(7u, rb.read(r, 7));
    TEST_ASSERT_EQUAL_MEMORY(w, r, 7);
  }
}

// ---------------------------------------------------------------------------
// Data integrity: sequential counter across thousands of bytes
// ---------------------------------------------------------------------------

static uint8_t backing_stress[4096];

void test_long_sequential_counter_integrity() {
  RingBuffer rb = make_buf(backing_stress, 4096);
  const size_t total_bytes = 100000;
  const size_t write_chunk = 200;
  const size_t read_chunk = 137;  // intentionally mismatched

  uint8_t wbuf[200];
  uint8_t rbuf[137];

  size_t written_total = 0;
  size_t read_total = 0;
  uint8_t write_counter = 0;
  uint8_t read_counter = 0;

  while (read_total < total_bytes) {
    // Write a chunk if there's space and we haven't hit the total yet
    if (written_total < total_bytes) {
      size_t to_write = write_chunk;
      if (written_total + to_write > total_bytes) {
        to_write = total_bytes - written_total;
      }
      for (size_t i = 0; i < to_write; ++i) {
        wbuf[i] = write_counter++;
      }
      size_t w = rb.write(wbuf, to_write);
      written_total += w;
      // If not all written, roll back counter for un-written bytes
      if (w < to_write) {
        write_counter = static_cast<uint8_t>(write_counter - (to_write - w));
      }
    }

    // Read a chunk and verify
    size_t r = rb.read(rbuf, read_chunk);
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(read_counter, rbuf[i]);
      read_counter++;
    }
    read_total += r;
  }

  TEST_ASSERT_EQUAL(total_bytes, read_total);
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// used() + free_space() invariant holds at every position during wrap cycles
// ---------------------------------------------------------------------------

void test_invariant_holds_during_wrap_cycles() {
  RingBuffer rb = make_buf(backing_medium, 64);

  for (int cycle = 0; cycle < 100; ++cycle) {
    uint8_t w[17];
    fill_pattern(w, 17, static_cast<uint8_t>(cycle));
    size_t written = rb.write(w, 17);
    TEST_ASSERT_EQUAL(63u, rb.used() + rb.free_space());

    uint8_t r[13];
    rb.read(r, 13);
    TEST_ASSERT_EQUAL(63u, rb.used() + rb.free_space());

    // Drain everything to prevent eventual overflow
    if (rb.used() > 40) {
      uint8_t drain[64];
      rb.read(drain, rb.used());
    }
    TEST_ASSERT_EQUAL(63u, rb.used() + rb.free_space());
  }
}

// ---------------------------------------------------------------------------
// Write 1 byte into every capacity-1 slot then verify full readback
// ---------------------------------------------------------------------------

void test_byte_by_byte_fill_then_read() {
  RingBuffer rb = make_buf(backing_medium, 64);  // 63 usable

  for (size_t i = 0; i < 63; ++i) {
    uint8_t b = static_cast<uint8_t>(i);
    TEST_ASSERT_EQUAL(1u, rb.write(&b, 1));
  }
  TEST_ASSERT_EQUAL(63u, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  for (size_t i = 0; i < 63; ++i) {
    uint8_t b;
    TEST_ASSERT_EQUAL(1u, rb.read(&b, 1));
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(i), b);
  }
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Interleaved variable-size write/read preserving data integrity
// ---------------------------------------------------------------------------

void test_variable_size_interleaved_integrity() {
  RingBuffer rb = make_buf(backing_large, 1024);
  uint8_t wbuf[256];
  uint8_t rbuf[256];

  // Use variable write sizes and read sizes, verify data
  const size_t write_sizes[] = {1, 3, 7, 15, 31, 63, 127, 255, 100, 50, 10, 2};
  const size_t read_sizes[] = {2, 5, 11, 23, 47, 99, 200, 64, 30, 8, 1, 4};
  const size_t num_sizes = sizeof(write_sizes) / sizeof(write_sizes[0]);

  size_t total_written = 0;
  size_t total_read = 0;

  for (int round = 0; round < 200; ++round) {
    size_t wsz = write_sizes[round % num_sizes];
    for (size_t i = 0; i < wsz; ++i) {
      wbuf[i] = static_cast<uint8_t>((total_written + i) & 0xFF);
    }
    size_t w = rb.write(wbuf, wsz);
    total_written += w;

    size_t rsz = read_sizes[round % num_sizes];
    size_t r = rb.read(rbuf, rsz);
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(
          static_cast<uint8_t>((total_read + i) & 0xFF), rbuf[i]);
    }
    total_read += r;
  }

  // Drain remainder
  while (rb.used() > 0) {
    size_t r = rb.read(rbuf, sizeof(rbuf));
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(
          static_cast<uint8_t>((total_read + i) & 0xFF), rbuf[i]);
    }
    total_read += r;
  }

  TEST_ASSERT_EQUAL(total_written, total_read);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  UNITY_BEGIN();

  // Basic
  RUN_TEST(test_empty_buffer_reports_zero_used);
  RUN_TEST(test_read_from_empty_returns_zero);
  RUN_TEST(test_write_then_read_round_trip);

  // Full / overflow
  RUN_TEST(test_write_fills_to_capacity_minus_one);
  RUN_TEST(test_write_to_full_buffer_returns_zero);
  RUN_TEST(test_write_partial_when_nearly_full);
  RUN_TEST(test_read_partial_when_less_available);

  // Wrap-around
  RUN_TEST(test_write_wraps_around);
  RUN_TEST(test_read_wraps_around);

  // Reset
  RUN_TEST(test_reset_clears_buffer);

  // Minimum capacity
  RUN_TEST(test_capacity_two_single_byte);

  // Single-byte cycling
  RUN_TEST(test_single_byte_write_read_cycle);

  // Streaming
  RUN_TEST(test_streaming_fill_drain_cycles);
  RUN_TEST(test_streaming_partial_drain);

  // Wrap boundary integrity
  RUN_TEST(test_wrap_boundary_data_integrity);

  // Zero-length ops
  RUN_TEST(test_write_zero_length);
  RUN_TEST(test_read_zero_length);

  // Invariant checks
  RUN_TEST(test_used_plus_free_equals_capacity_minus_one);

  // Fill-drain-refill
  RUN_TEST(test_fill_drain_refill);

  // FIFO order
  RUN_TEST(test_fifo_order_with_interleaved_ops);

  // Edge: write lands exactly at buffer end
  RUN_TEST(test_write_lands_exactly_at_end);

  // Null / degenerate input guards
  RUN_TEST(test_write_null_data_returns_zero);
  RUN_TEST(test_read_null_data_returns_zero);
  RUN_TEST(test_null_storage_write_returns_zero);
  RUN_TEST(test_null_storage_read_returns_zero);
  RUN_TEST(test_capacity_zero_is_safe);
  RUN_TEST(test_capacity_one_is_safe);

  // Read lands exactly at buffer end
  RUN_TEST(test_read_lands_exactly_at_end);

  // Multiple / post-wrap resets
  RUN_TEST(test_multiple_resets_are_safe);
  RUN_TEST(test_reset_after_wrap);

  // Overflow: write larger than capacity
  RUN_TEST(test_write_larger_than_capacity_writes_partial);

  // High-water single-byte cycling
  RUN_TEST(test_near_full_single_byte_cycling);

  // Full fill at every start position
  RUN_TEST(test_full_fill_at_every_start_position);

  // Non-power-of-2 capacity
  RUN_TEST(test_non_power_of_two_capacity);

  // Long sequential counter integrity (100 KB streamed)
  RUN_TEST(test_long_sequential_counter_integrity);

  // Invariant during wrap cycles
  RUN_TEST(test_invariant_holds_during_wrap_cycles);

  // Byte-by-byte fill then read
  RUN_TEST(test_byte_by_byte_fill_then_read);

  // Variable-size interleaved integrity
  RUN_TEST(test_variable_size_interleaved_integrity);

  return UNITY_END();
}
