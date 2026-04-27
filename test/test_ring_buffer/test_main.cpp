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
// Concurrent-style stress: rapid alternating write/read with prime chunks
// ---------------------------------------------------------------------------

void test_prime_chunk_stress() {
  RingBuffer rb = make_buf(backing_large, 1024);  // 1023 usable

  // Use prime-sized chunks to maximise wrap collisions: writes and reads
  // constantly cross the storage boundary at different offsets.
  const size_t write_prime = 127;
  const size_t read_prime = 113;
  uint8_t wbuf[127];
  uint8_t rbuf[113];

  uint8_t write_seq = 0;
  uint8_t read_seq = 0;
  size_t total_written = 0;
  size_t total_read = 0;
  const size_t target = 50000;

  while (total_written < target) {
    // Write a prime-sized chunk
    for (size_t i = 0; i < write_prime; ++i)
      wbuf[i] = write_seq++;
    size_t w = rb.write(wbuf, write_prime);
    total_written += w;
    if (w < write_prime)
      write_seq = static_cast<uint8_t>(write_seq - (write_prime - w));

    // Read a different prime-sized chunk
    size_t r = rb.read(rbuf, read_prime);
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(read_seq, rbuf[i]);
      read_seq++;
    }
    total_read += r;
  }

  // Drain remaining bytes from the buffer
  while (rb.used() > 0) {
    size_t r = rb.read(rbuf, read_prime);
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(read_seq, rbuf[i]);
      read_seq++;
    }
    total_read += r;
  }

  TEST_ASSERT_EQUAL(total_written, total_read);
}

// ---------------------------------------------------------------------------
// Exactly capacity-minus-one write then read (boundary)
// ---------------------------------------------------------------------------

void test_exact_capacity_minus_one_write_read() {
  // Use medium buffer: capacity 64, usable 63
  RingBuffer rb = make_buf(backing_medium, 64);
  const size_t usable = 63;

  uint8_t data[63];
  fill_pattern(data, usable, 0xDE);

  // Write exactly usable bytes
  TEST_ASSERT_EQUAL(usable, rb.write(data, usable));
  TEST_ASSERT_EQUAL(usable, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  // Read exactly usable bytes
  uint8_t out[63];
  TEST_ASSERT_EQUAL(usable, rb.read(out, usable));
  TEST_ASSERT_EQUAL_MEMORY(data, out, usable);
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(usable, rb.free_space());
}

// ---------------------------------------------------------------------------
// Write that returns partial, then verify remaining free_space is exactly 0
// ---------------------------------------------------------------------------

void test_partial_write_leaves_zero_free() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable

  // Fill 5 of 7 usable bytes
  uint8_t initial[5];
  fill_pattern(initial, 5, 0x01);
  TEST_ASSERT_EQUAL(5u, rb.write(initial, 5));
  TEST_ASSERT_EQUAL(2u, rb.free_space());

  // Attempt to write 10 bytes -- only 2 should fit
  uint8_t big[10];
  fill_pattern(big, 10, 0xA0);
  size_t written = rb.write(big, 10);
  TEST_ASSERT_EQUAL(2u, written);
  TEST_ASSERT_EQUAL(0u, rb.free_space());
  TEST_ASSERT_EQUAL(7u, rb.used());
}

// ---------------------------------------------------------------------------
// Multiple sequential partial writes filling to capacity
// ---------------------------------------------------------------------------

void test_multiple_partial_writes_to_capacity() {
  RingBuffer rb = make_buf(backing_medium, 64);  // 63 usable
  const size_t usable = 63;

  // Write in chunks of 10 until full
  uint8_t chunk[10];
  size_t total = 0;
  size_t pass = 0;

  while (total < usable) {
    fill_pattern(chunk, 10, static_cast<uint8_t>(pass * 10));
    size_t w = rb.write(chunk, 10);
    total += w;
    pass++;

    // If we got a partial write (w < 10), buffer must now be full
    if (w < 10) {
      TEST_ASSERT_EQUAL(0u, rb.free_space());
    }
  }

  TEST_ASSERT_EQUAL(usable, total);
  TEST_ASSERT_EQUAL(usable, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  // Verify we did 7 passes: 6 full (60 bytes) + 1 partial (3 bytes)
  TEST_ASSERT_EQUAL(7u, pass);
}

// ---------------------------------------------------------------------------
// Read more bytes than buffer has ever held
// ---------------------------------------------------------------------------

void test_read_more_than_ever_written() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable

  // Write 3 bytes
  uint8_t data[] = {0x10, 0x20, 0x30};
  rb.write(data, 3);

  // Try to read 100 bytes -- should return only 3
  uint8_t out[100] = {};
  size_t r = rb.read(out, 100);
  TEST_ASSERT_EQUAL(3u, r);
  TEST_ASSERT_EQUAL_HEX8(0x10, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0x20, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0x30, out[2]);
  TEST_ASSERT_EQUAL(0u, rb.used());

  // Another read should return 0
  TEST_ASSERT_EQUAL(0u, rb.read(out, 100));
}

// ---------------------------------------------------------------------------
// Reset at various head/tail positions verifies head==0 and tail==0
// ---------------------------------------------------------------------------

void test_reset_at_various_positions() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance to several different head/tail positions and reset each time
  const size_t advances[] = {1, 7, 13, 31, 60, 63};
  for (size_t adv : advances) {
    rb.reset();
    // Advance head and tail to position (adv % capacity)
    uint8_t tmp[63];
    fill_pattern(tmp, adv, 0);
    rb.write(tmp, adv);
    rb.read(tmp, adv);

    // Head and tail are now at some non-zero position
    // Write a few more bytes to make the buffer non-empty
    uint8_t extra[] = {0xAA, 0xBB};
    rb.write(extra, 2);

    rb.reset();
    TEST_ASSERT_EQUAL(0u, rb.head);
    TEST_ASSERT_EQUAL(0u, rb.tail);
    TEST_ASSERT_EQUAL(0u, rb.used());
    TEST_ASSERT_EQUAL(63u, rb.free_space());
  }
}

// ---------------------------------------------------------------------------
// Capacity==2 with many single-byte cycles (1-usable-byte edge)
// ---------------------------------------------------------------------------

static uint8_t backing_cap2[2];

void test_capacity_two_many_cycles() {
  RingBuffer rb = make_buf(backing_cap2, 2);  // 1 usable byte

  // Cycle single bytes for many iterations
  for (uint8_t i = 0; i < 200; ++i) {
    TEST_ASSERT_EQUAL(0u, rb.used());
    TEST_ASSERT_EQUAL(1u, rb.free_space());

    uint8_t w = i;
    TEST_ASSERT_EQUAL(1u, rb.write(&w, 1));
    TEST_ASSERT_EQUAL(1u, rb.used());
    TEST_ASSERT_EQUAL(0u, rb.free_space());

    // Second write must fail
    uint8_t extra = 0xFF;
    TEST_ASSERT_EQUAL(0u, rb.write(&extra, 1));

    uint8_t r = 0;
    TEST_ASSERT_EQUAL(1u, rb.read(&r, 1));
    TEST_ASSERT_EQUAL_HEX8(i, r);
    TEST_ASSERT_EQUAL(0u, rb.used());
  }
}

// ---------------------------------------------------------------------------
// Simulate firmware drain pattern: write large, read fixed 1024-byte chunks
// ---------------------------------------------------------------------------

void test_firmware_drain_pattern() {
  // Mimics main.cpp: TCP writes variable sizes, drain task reads kDrainChunkSize=1024
  RingBuffer rb = make_buf(backing_stress, 4096);
  const size_t drain_chunk = 1024;
  uint8_t wbuf[4096];
  uint8_t rbuf[1024];

  // Simulate TCP bursts of varying sizes writing into buffer
  const size_t tcp_sizes[] = {1460, 2920, 536, 4096, 1, 1023, 1024, 1025, 2048};
  uint8_t write_counter = 0;
  uint8_t read_counter = 0;
  size_t total_written = 0;
  size_t total_read = 0;

  for (int round = 0; round < 200; ++round) {
    size_t tcp_burst = tcp_sizes[round % 9];
    for (size_t i = 0; i < tcp_burst; ++i)
      wbuf[i] = write_counter++;
    size_t w = rb.write(wbuf, tcp_burst);
    total_written += w;
    if (w < tcp_burst)
      write_counter = static_cast<uint8_t>(write_counter - (tcp_burst - w));

    // Drain in 1024-byte chunks like the firmware does
    while (rb.used() >= drain_chunk) {
      size_t r = rb.read(rbuf, drain_chunk);
      for (size_t i = 0; i < r; ++i) {
        TEST_ASSERT_EQUAL_HEX8(read_counter, rbuf[i]);
        read_counter++;
      }
      total_read += r;
    }
  }

  // Final drain
  while (rb.used() > 0) {
    size_t r = rb.read(rbuf, drain_chunk);
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(read_counter, rbuf[i]);
      read_counter++;
    }
    total_read += r;
  }
  TEST_ASSERT_EQUAL(total_written, total_read);
}

// ---------------------------------------------------------------------------
// Write exactly free_space() bytes — should succeed and leave buffer full
// ---------------------------------------------------------------------------

void test_write_exact_free_space() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Partially fill
  uint8_t tmp[20];
  fill_pattern(tmp, 20, 0xA0);
  rb.write(tmp, 20);

  const size_t free = rb.free_space();
  TEST_ASSERT_EQUAL(43u, free);  // 63 - 20

  uint8_t data[43];
  fill_pattern(data, 43, 0xB0);
  TEST_ASSERT_EQUAL(43u, rb.write(data, 43));
  TEST_ASSERT_EQUAL(0u, rb.free_space());
  TEST_ASSERT_EQUAL(63u, rb.used());

  // One more byte must fail
  uint8_t extra = 0xFF;
  TEST_ASSERT_EQUAL(0u, rb.write(&extra, 1));
}

// ---------------------------------------------------------------------------
// Alternating 1-byte write and large read (consumer faster than producer)
// ---------------------------------------------------------------------------

void test_slow_producer_fast_consumer() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t read_buf[64];

  for (uint8_t i = 0; i < 200; ++i) {
    uint8_t w = i;
    TEST_ASSERT_EQUAL(1u, rb.write(&w, 1));

    // Consumer tries to read a large chunk but only gets 1
    size_t r = rb.read(read_buf, 64);
    TEST_ASSERT_EQUAL(1u, r);
    TEST_ASSERT_EQUAL_HEX8(i, read_buf[0]);
    TEST_ASSERT_EQUAL(0u, rb.used());
  }
}

// ---------------------------------------------------------------------------
// Rapid fill-to-full / drain-to-empty cycles (simulates job start/end)
// ---------------------------------------------------------------------------

void test_rapid_fill_drain_full_cycles() {
  RingBuffer rb = make_buf(backing_large, 1024);  // 1023 usable
  uint8_t data[1023];
  uint8_t out[1023];

  for (int cycle = 0; cycle < 50; ++cycle) {
    fill_pattern(data, 1023, static_cast<uint8_t>(cycle * 3));

    // Fill completely
    TEST_ASSERT_EQUAL(1023u, rb.write(data, 1023));
    TEST_ASSERT_EQUAL(0u, rb.free_space());
    TEST_ASSERT_EQUAL(1023u, rb.used());

    // Drain completely
    TEST_ASSERT_EQUAL(1023u, rb.read(out, 1023));
    TEST_ASSERT_EQUAL_MEMORY(data, out, 1023);
    TEST_ASSERT_EQUAL(0u, rb.used());
    TEST_ASSERT_EQUAL(1023u, rb.free_space());
  }
}

// ---------------------------------------------------------------------------
// Write that exactly fills to_end (first memcpy uses all space to end)
// ---------------------------------------------------------------------------

void test_write_fills_exactly_to_end() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance head to position 50, tail to 50
  uint8_t tmp[50];
  fill_pattern(tmp, 50, 0);
  rb.write(tmp, 50);
  rb.read(tmp, 50);

  // to_end = capacity - head = 64 - 50 = 14
  // Write exactly 14 bytes — first memcpy copies all, no second memcpy needed
  uint8_t data[14];
  fill_pattern(data, 14, 0xC0);
  TEST_ASSERT_EQUAL(14u, rb.write(data, 14));
  // head should now be at 0
  TEST_ASSERT_EQUAL(14u, rb.used());

  uint8_t out[14];
  TEST_ASSERT_EQUAL(14u, rb.read(out, 14));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 14);
}

// ---------------------------------------------------------------------------
// Read that exactly consumes to_end (first memcpy reads all to end)
// ---------------------------------------------------------------------------

void test_read_consumes_exactly_to_end() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance tail to position 50
  uint8_t tmp[50];
  fill_pattern(tmp, 50, 0);
  rb.write(tmp, 50);
  rb.read(tmp, 50);

  // Write 20 bytes wrapping: positions 50..63 (14 bytes) + 0..5 (6 bytes)
  uint8_t data[20];
  fill_pattern(data, 20, 0xD0);
  rb.write(data, 20);

  // to_end = capacity - tail = 64 - 50 = 14
  // Read exactly 14 — should get first 14 bytes of data (the to-end portion)
  uint8_t out[14];
  TEST_ASSERT_EQUAL(14u, rb.read(out, 14));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 14);
  // tail should now be at 0, 6 bytes remaining
  TEST_ASSERT_EQUAL(6u, rb.used());

  uint8_t out2[6];
  TEST_ASSERT_EQUAL(6u, rb.read(out2, 6));
  TEST_ASSERT_EQUAL_MEMORY(data + 14, out2, 6);
}

// ---------------------------------------------------------------------------
// Simulate prefill pattern: fill to threshold, then interleave write/read
// ---------------------------------------------------------------------------

void test_prefill_then_interleaved_streaming() {
  RingBuffer rb = make_buf(backing_stress, 4096);  // 4095 usable
  const size_t prefill_target = 2048;
  uint8_t wbuf[256];
  uint8_t rbuf[256];
  uint8_t counter = 0;
  uint8_t read_counter = 0;
  size_t total_written = 0;

  // Phase 1: prefill without reading (like the firmware pre-fill gate)
  while (total_written < prefill_target) {
    for (size_t i = 0; i < 256; ++i)
      wbuf[i] = counter++;
    size_t w = rb.write(wbuf, 256);
    total_written += w;
    if (w < 256)
      counter = static_cast<uint8_t>(counter - (256 - w));
  }
  TEST_ASSERT_TRUE(rb.used() >= prefill_target);

  // Phase 2: interleaved write/read (like normal streaming after prefill)
  size_t total_read = 0;
  for (int i = 0; i < 300; ++i) {
    for (size_t j = 0; j < 128; ++j)
      wbuf[j] = counter++;
    size_t w = rb.write(wbuf, 128);
    total_written += w;
    if (w < 128)
      counter = static_cast<uint8_t>(counter - (128 - w));

    size_t r = rb.read(rbuf, 200);
    for (size_t j = 0; j < r; ++j) {
      TEST_ASSERT_EQUAL_HEX8(read_counter, rbuf[j]);
      read_counter++;
    }
    total_read += r;
  }

  // Final drain
  while (rb.used() > 0) {
    size_t r = rb.read(rbuf, 256);
    for (size_t j = 0; j < r; ++j) {
      TEST_ASSERT_EQUAL_HEX8(read_counter, rbuf[j]);
      read_counter++;
    }
    total_read += r;
  }
  TEST_ASSERT_EQUAL(total_written, total_read);
}

// ---------------------------------------------------------------------------
// Back-to-back reset then immediate full write (simulates rapid job cycling)
// ---------------------------------------------------------------------------

void test_reset_then_immediate_full_write() {
  RingBuffer rb = make_buf(backing_medium, 64);  // 63 usable
  uint8_t data[63];
  uint8_t out[63];

  for (int cycle = 0; cycle < 100; ++cycle) {
    // Partially fill, then reset mid-stream (simulates job abort)
    uint8_t partial[30];
    fill_pattern(partial, 30, static_cast<uint8_t>(cycle));
    rb.write(partial, 30);
    rb.reset();

    // Immediately start new job with full write
    fill_pattern(data, 63, static_cast<uint8_t>(cycle + 100));
    TEST_ASSERT_EQUAL(63u, rb.write(data, 63));
    TEST_ASSERT_EQUAL(63u, rb.read(out, 63));
    TEST_ASSERT_EQUAL_MEMORY(data, out, 63);
    TEST_ASSERT_EQUAL(0u, rb.used());
    rb.reset();
  }
}

// ---------------------------------------------------------------------------
// Write SIZE_MAX length (should not crash, should write at most free_space)
// ---------------------------------------------------------------------------

void test_write_size_max_no_crash() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable
  uint8_t data[7];
  fill_pattern(data, 7, 0xAA);

  // Attempt to write SIZE_MAX bytes — the min(len, avail) logic should clamp
  // to free_space (7). We pass a valid pointer but lie about length.
  // The ring buffer should write at most 7 bytes.
  size_t written = rb.write(data, SIZE_MAX);
  TEST_ASSERT_EQUAL(7u, written);
  TEST_ASSERT_EQUAL(7u, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());
}

// ---------------------------------------------------------------------------
// used() consistency when head < tail (wrap state)
// ---------------------------------------------------------------------------

void test_used_when_head_less_than_tail() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance to tail=6, head=6
  uint8_t tmp[6];
  fill_pattern(tmp, 6, 0);
  rb.write(tmp, 6);
  rb.read(tmp, 6);

  // Write 4 bytes: positions 6,7,0,1 → head=2, tail=6
  uint8_t data[4];
  fill_pattern(data, 4, 0xE0);
  TEST_ASSERT_EQUAL(4u, rb.write(data, 4));
  // head=2, tail=6 → head < tail
  TEST_ASSERT_EQUAL(4u, rb.used());
  TEST_ASSERT_EQUAL(3u, rb.free_space());
  // Invariant still holds
  TEST_ASSERT_EQUAL(7u, rb.used() + rb.free_space());

  uint8_t out[4];
  TEST_ASSERT_EQUAL(4u, rb.read(out, 4));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 4);
}

// ---------------------------------------------------------------------------
// Repeated partial writes then single large read (batch accumulation pattern)
// ---------------------------------------------------------------------------

void test_batch_accumulation_then_drain() {
  RingBuffer rb = make_buf(backing_large, 1024);  // 1023 usable
  uint8_t counter = 0;

  // Accumulate many small writes (like many small TCP packets)
  for (int i = 0; i < 500; ++i) {
    uint8_t byte = counter++;
    rb.write(&byte, 1);
  }
  TEST_ASSERT_EQUAL(500u, rb.used());

  // Single large read (like drain task reading chunk)
  uint8_t out[500];
  TEST_ASSERT_EQUAL(500u, rb.read(out, 500));
  for (int i = 0; i < 500; ++i) {
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(i), out[i]);
  }
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// free_space() returns 0 for degenerate capacities
// ---------------------------------------------------------------------------

void test_free_space_degenerate() {
  RingBuffer rb;
  rb.storage = backing_small;
  rb.head = 0;
  rb.tail = 0;

  rb.capacity = 0;
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  rb.capacity = 1;
  TEST_ASSERT_EQUAL(0u, rb.free_space());
}

// ---------------------------------------------------------------------------
// Stale data not leaked after reset (storage retains bytes but they're gone)
// ---------------------------------------------------------------------------

void test_reset_does_not_leak_stale_data() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Fill with a recognizable pattern
  uint8_t data[63];
  fill_pattern(data, 63, 0xA0);
  rb.write(data, 63);

  // Reset
  rb.reset();
  TEST_ASSERT_EQUAL(0u, rb.used());

  // Read from empty buffer should produce nothing (not the stale 0xA0 bytes)
  uint8_t out[63];
  std::memset(out, 0, sizeof(out));
  TEST_ASSERT_EQUAL(0u, rb.read(out, 63));
  // All out bytes should remain 0 (untouched)
  for (size_t i = 0; i < 63; ++i) {
    TEST_ASSERT_EQUAL_HEX8(0, out[i]);
  }
}

// ---------------------------------------------------------------------------
// Write capacity bytes (one more than usable) — must write exactly capacity-1
// ---------------------------------------------------------------------------

void test_write_capacity_bytes_writes_capacity_minus_one() {
  RingBuffer rb = make_buf(backing_small, 8);  // capacity=8, usable=7
  uint8_t data[8];
  fill_pattern(data, 8, 0xC0);

  // Ask to write all 8 bytes — should write only 7
  TEST_ASSERT_EQUAL(7u, rb.write(data, 8));
  TEST_ASSERT_EQUAL(7u, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  // Verify contents are the FIRST 7 bytes of data
  uint8_t out[7];
  TEST_ASSERT_EQUAL(7u, rb.read(out, 7));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 7);  // data[0..6], not data[1..7]
}

// ---------------------------------------------------------------------------
// Many fill-drain cycles: storage region should not get progressively corrupted
// ---------------------------------------------------------------------------

void test_many_cycles_no_storage_corruption() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t data[63];
  uint8_t out[63];

  for (int cycle = 0; cycle < 1000; ++cycle) {
    // Fill with cycle-specific pattern
    const uint8_t seed = static_cast<uint8_t>(cycle & 0xFF);
    fill_pattern(data, 63, seed);
    TEST_ASSERT_EQUAL(63u, rb.write(data, 63));

    // Drain and verify
    TEST_ASSERT_EQUAL(63u, rb.read(out, 63));
    for (size_t i = 0; i < 63; ++i) {
      TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(seed + i), out[i]);
    }
  }
}

// ---------------------------------------------------------------------------
// Sentinel byte edge: at every possible head position, verify one-byte-free
// ---------------------------------------------------------------------------

void test_sentinel_at_every_position() {
  RingBuffer rb = make_buf(backing_small, 8);  // 7 usable

  // For each possible starting position (0..7), fill completely and confirm
  // free_space is 0 and an extra write fails.
  for (size_t pos = 0; pos < 8; ++pos) {
    rb.reset();
    // Advance head and tail to `pos`
    if (pos > 0) {
      uint8_t tmp[7];
      fill_pattern(tmp, pos, 0);
      rb.write(tmp, pos);
      rb.read(tmp, pos);
    }

    // Fill completely
    uint8_t data[7];
    fill_pattern(data, 7, static_cast<uint8_t>(0x10 + pos));
    TEST_ASSERT_EQUAL(7u, rb.write(data, 7));

    // At this point: head==tail-1 modulo capacity; free should be 0
    TEST_ASSERT_EQUAL(0u, rb.free_space());
    TEST_ASSERT_EQUAL(7u, rb.used());

    // head and tail should differ by exactly 1 (sentinel)
    // head == (pos + 7) % 8, tail == pos
    const size_t expected_head = (pos + 7) % 8;
    TEST_ASSERT_EQUAL(expected_head, rb.head);
    TEST_ASSERT_EQUAL(pos, rb.tail);

    uint8_t extra = 0xFF;
    TEST_ASSERT_EQUAL(0u, rb.write(&extra, 1));
  }
}

// ---------------------------------------------------------------------------
// Write exactly one byte at a time, drain all at once, verify order
// ---------------------------------------------------------------------------

void test_single_byte_writes_bulk_drain_keeps_order() {
  RingBuffer rb = make_buf(backing_large, 1024);  // 1023 usable
  const size_t n = 1023;

  for (size_t i = 0; i < n; ++i) {
    uint8_t b = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    TEST_ASSERT_EQUAL(1u, rb.write(&b, 1));
  }
  TEST_ASSERT_EQUAL(n, rb.used());
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  uint8_t out[1023];
  TEST_ASSERT_EQUAL(n, rb.read(out, n));
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>((i * 7 + 3) & 0xFF), out[i]);
  }
}

// ---------------------------------------------------------------------------
// Tail wraps to 0 during a non-wrapping read (read lands exactly at capacity)
// ---------------------------------------------------------------------------

void test_tail_wraps_to_zero_on_non_wrapping_read() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance tail to 3
  uint8_t tmp[3];
  fill_pattern(tmp, 3, 0);
  rb.write(tmp, 3);
  rb.read(tmp, 3);

  // Write 5 bytes at positions 3,4,5,6,7 (fills to end, no wrap on head)
  uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  TEST_ASSERT_EQUAL(5u, rb.write(data, 5));
  // head should now be at 0 (wrapped from 8)

  // Read all 5 — tail goes 3→8, which wraps to 0
  uint8_t out[5];
  TEST_ASSERT_EQUAL(5u, rb.read(out, 5));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 5);
  TEST_ASSERT_EQUAL(0u, rb.tail);
  TEST_ASSERT_EQUAL(0u, rb.head);
}

// ---------------------------------------------------------------------------
// Asymmetric producer/consumer rates (producer 2x faster than consumer)
// ---------------------------------------------------------------------------

void test_producer_faster_than_consumer_backpressure() {
  RingBuffer rb = make_buf(backing_large, 1024);  // 1023 usable

  uint8_t wbuf[200];
  uint8_t rbuf[100];
  uint8_t write_seq = 0;
  uint8_t read_seq = 0;
  size_t total_written = 0;
  size_t total_read = 0;
  size_t write_attempts = 0;
  size_t write_full = 0;

  // Producer writes 200 bytes at a time, consumer reads 100 — buffer fills up
  // and producer gets partial/zero writes (backpressure).
  for (int i = 0; i < 100; ++i) {
    for (size_t j = 0; j < 200; ++j) wbuf[j] = write_seq++;
    size_t w = rb.write(wbuf, 200);
    total_written += w;
    write_attempts++;
    if (w == 0) write_full++;
    if (w < 200)
      write_seq = static_cast<uint8_t>(write_seq - (200 - w));

    size_t r = rb.read(rbuf, 100);
    for (size_t j = 0; j < r; ++j) {
      TEST_ASSERT_EQUAL_HEX8(read_seq, rbuf[j]);
      read_seq++;
    }
    total_read += r;
  }

  // Drain remainder
  while (rb.used() > 0) {
    size_t r = rb.read(rbuf, 100);
    for (size_t j = 0; j < r; ++j) {
      TEST_ASSERT_EQUAL_HEX8(read_seq, rbuf[j]);
      read_seq++;
    }
    total_read += r;
  }

  TEST_ASSERT_EQUAL(total_written, total_read);
  // Verify backpressure actually kicked in (at least some writes saturated)
  TEST_ASSERT_TRUE(total_written < 200 * 100);  // less than unlimited
}

// ---------------------------------------------------------------------------
// Large single read when data is split across wrap boundary
// ---------------------------------------------------------------------------

void test_large_read_straddling_wrap() {
  RingBuffer rb = make_buf(backing_large, 1024);  // 1023 usable

  // Position tail near end: write 1000, read 1000 → tail=1000, head=1000
  uint8_t tmp[1000];
  fill_pattern(tmp, 1000, 0);
  rb.write(tmp, 1000);
  rb.read(tmp, 1000);

  // Write 500 bytes that straddle the wrap (24 before end, 476 after)
  uint8_t data[500];
  for (size_t i = 0; i < 500; ++i)
    data[i] = static_cast<uint8_t>((i * 11 + 5) & 0xFF);
  TEST_ASSERT_EQUAL(500u, rb.write(data, 500));

  // Single large read verifies data reconstructed correctly across wrap
  uint8_t out[500];
  TEST_ASSERT_EQUAL(500u, rb.read(out, 500));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 500);
}

// ---------------------------------------------------------------------------
// Interleaved write+reset+write pattern (job abort mid-write)
// ---------------------------------------------------------------------------

void test_write_reset_write_discards_pre_reset_data() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Write first batch
  uint8_t first[] = {0xDE, 0xAD, 0xBE, 0xEF};
  rb.write(first, 4);
  TEST_ASSERT_EQUAL(4u, rb.used());

  // Reset — first batch must be discarded
  rb.reset();
  TEST_ASSERT_EQUAL(0u, rb.used());

  // Write second batch
  uint8_t second[] = {0xCA, 0xFE};
  rb.write(second, 2);

  // Read should only yield the second batch, not the first
  uint8_t out[6] = {0};
  TEST_ASSERT_EQUAL(2u, rb.read(out, 6));
  TEST_ASSERT_EQUAL_HEX8(0xCA, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFE, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0, out[2]);  // untouched
}

// ---------------------------------------------------------------------------
// Write then read in smaller chunks verifies second-memcpy logic
// ---------------------------------------------------------------------------

void test_second_memcpy_correctness() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Force a wrap during both write and read:
  // advance head/tail to 6
  uint8_t tmp[6];
  rb.write(tmp, 6);
  rb.read(tmp, 6);

  // Write 5 bytes: 2 fit at positions 6,7, then 3 wrap to 0,1,2
  uint8_t data[] = {0x11, 0x22, 0x33, 0x44, 0x55};
  TEST_ASSERT_EQUAL(5u, rb.write(data, 5));

  // Read them in 3 separate calls and verify each byte
  uint8_t a, b, c;
  TEST_ASSERT_EQUAL(1u, rb.read(&a, 1));
  TEST_ASSERT_EQUAL(1u, rb.read(&b, 1));
  TEST_ASSERT_EQUAL(1u, rb.read(&c, 1));
  TEST_ASSERT_EQUAL_HEX8(0x11, a);
  TEST_ASSERT_EQUAL_HEX8(0x22, b);
  TEST_ASSERT_EQUAL_HEX8(0x33, c);

  uint8_t de[2];
  TEST_ASSERT_EQUAL(2u, rb.read(de, 2));
  TEST_ASSERT_EQUAL_HEX8(0x44, de[0]);
  TEST_ASSERT_EQUAL_HEX8(0x55, de[1]);
}

// ---------------------------------------------------------------------------
// used() math verified at every head/tail combination for a small buffer
// ---------------------------------------------------------------------------

void test_used_invariant_exhaustive_small() {
  // Exhaustive: for every possible (head, tail) pair in a small buffer,
  // verify used() + free_space() == capacity - 1 (when capacity >= 2).
  for (size_t cap = 2; cap <= 16; ++cap) {
    uint8_t tmp_storage[16];
    RingBuffer rb;
    rb.storage = tmp_storage;
    rb.capacity = cap;
    for (size_t h = 0; h < cap; ++h) {
      for (size_t t = 0; t < cap; ++t) {
        rb.head = h;
        rb.tail = t;
        const size_t expected_used =
            (h >= t) ? (h - t) : (cap - t + h);
        TEST_ASSERT_EQUAL(expected_used, rb.used());
        TEST_ASSERT_EQUAL(cap - 1, rb.used() + rb.free_space());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Fill buffer, drain by 1, write by 1 — sentinel stays consistent (N iterations)
// ---------------------------------------------------------------------------

void test_full_buffer_one_byte_cycling_integrity() {
  RingBuffer rb = make_buf(backing_medium, 64);  // 63 usable

  // Fill with an identifiable pattern
  uint8_t initial[63];
  fill_pattern(initial, 63, 0x00);
  rb.write(initial, 63);

  uint8_t expected_head_byte = 0;

  // Cycle: read 1 (should be expected head byte), write 1 new
  for (int i = 0; i < 500; ++i) {
    uint8_t read_byte;
    TEST_ASSERT_EQUAL(1u, rb.read(&read_byte, 1));
    TEST_ASSERT_EQUAL_HEX8(expected_head_byte, read_byte);
    expected_head_byte++;

    uint8_t write_byte = static_cast<uint8_t>(63 + i);
    TEST_ASSERT_EQUAL(1u, rb.write(&write_byte, 1));

    TEST_ASSERT_EQUAL(63u, rb.used());
    TEST_ASSERT_EQUAL(0u, rb.free_space());
  }
}

// ---------------------------------------------------------------------------
// Write into buffer that already has wrapped data — verify correct placement
// ---------------------------------------------------------------------------

void test_write_after_wrap_state() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance to position 60
  uint8_t tmp[60];
  fill_pattern(tmp, 60, 0);
  rb.write(tmp, 60);
  rb.read(tmp, 60);

  // Write 10 bytes straddling wrap (4 at end, 6 at start) — head now at 6
  uint8_t first[10];
  fill_pattern(first, 10, 0xA0);
  rb.write(first, 10);

  // Write 20 more bytes — no wrap this time (positions 6..25)
  uint8_t second[20];
  fill_pattern(second, 20, 0xB0);
  rb.write(second, 20);
  TEST_ASSERT_EQUAL(30u, rb.used());

  // Read back all 30 — should be first 10, then second 20, in order
  uint8_t out[30];
  TEST_ASSERT_EQUAL(30u, rb.read(out, 30));
  TEST_ASSERT_EQUAL_MEMORY(first, out, 10);
  TEST_ASSERT_EQUAL_MEMORY(second, out + 10, 20);
}

// ---------------------------------------------------------------------------
// Null + zero-length combinations (defensive guard interactions)
// ---------------------------------------------------------------------------

void test_write_null_and_zero_length() {
  RingBuffer rb = make_buf(backing_small, 8);
  TEST_ASSERT_EQUAL(0u, rb.write(nullptr, 0));
  TEST_ASSERT_EQUAL(0u, rb.used());
}

void test_read_null_and_zero_length() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {1, 2, 3};
  rb.write(data, 3);
  TEST_ASSERT_EQUAL(0u, rb.read(nullptr, 0));
  TEST_ASSERT_EQUAL(3u, rb.used());
}

void test_write_with_null_storage_and_null_data() {
  RingBuffer rb;
  rb.storage = nullptr;
  rb.capacity = 64;
  rb.head = 0;
  rb.tail = 0;
  TEST_ASSERT_EQUAL(0u, rb.write(nullptr, 10));
  TEST_ASSERT_EQUAL(0u, rb.used());
}

void test_read_with_null_storage_and_null_data() {
  RingBuffer rb;
  rb.storage = nullptr;
  rb.capacity = 64;
  rb.head = 0;
  rb.tail = 0;
  TEST_ASSERT_EQUAL(0u, rb.read(nullptr, 10));
}

// ---------------------------------------------------------------------------
// Smallest non-trivial capacity: 3 (2 usable bytes)
// ---------------------------------------------------------------------------

void test_capacity_three_edge_behavior() {
  uint8_t tiny[3];
  RingBuffer rb = make_buf(tiny, 3);
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(2u, rb.free_space());

  // Fill fully
  uint8_t data[] = {0xA1, 0xA2};
  TEST_ASSERT_EQUAL(2u, rb.write(data, 2));
  TEST_ASSERT_EQUAL(0u, rb.free_space());

  // Over-write rejected
  uint8_t extra[] = {0xFF, 0xFF};
  TEST_ASSERT_EQUAL(0u, rb.write(extra, 2));

  // Partial drain + refill exercises wrap at small scale
  uint8_t out1;
  TEST_ASSERT_EQUAL(1u, rb.read(&out1, 1));
  TEST_ASSERT_EQUAL_HEX8(0xA1, out1);
  uint8_t newer[] = {0xB1};
  TEST_ASSERT_EQUAL(1u, rb.write(newer, 1));
  TEST_ASSERT_EQUAL(2u, rb.used());

  // Drain remaining — must be in FIFO order
  uint8_t out[2];
  TEST_ASSERT_EQUAL(2u, rb.read(out, 2));
  TEST_ASSERT_EQUAL_HEX8(0xA2, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xB1, out[1]);
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Failed / partial write must NOT mutate storage past the actually-written span
// ---------------------------------------------------------------------------

void test_partial_write_does_not_touch_past_free_region() {
  // Pre-poison storage, partially fill, then verify bytes beyond the write
  // span remain untouched.  This is about the write not overrunning its own
  // `n` count — not about the sentinel, which is a logical-only constraint.
  uint8_t storage[16];
  std::memset(storage, 0x5A, sizeof(storage));
  RingBuffer rb;
  rb.storage = storage;
  rb.capacity = 16;
  rb.head = 0;
  rb.tail = 0;

  // Write 5 bytes — touches storage[0..4] only
  uint8_t data[5] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4};
  rb.write(data, 5);
  for (size_t i = 5; i < 16; ++i) {
    TEST_ASSERT_EQUAL_HEX8(0x5A, storage[i]);
  }

  // Oversize partial: prime near-full and attempt to write 50 bytes.
  // Only free_space should be written, not more.
  rb.reset();
  std::memset(storage, 0x5A, sizeof(storage));
  uint8_t prime[10];
  fill_pattern(prime, 10, 0x70);
  rb.write(prime, 10);  // storage[0..9] written, [10..15] still 0x5A
  // free_space = 15 - 10 = 5.  Request 50 — only 5 should land.
  uint8_t big[50];
  fill_pattern(big, 50, 0xC0);
  const size_t w = rb.write(big, 50);
  TEST_ASSERT_EQUAL(5u, w);
  // The 5 written bytes go to storage[10..14].  storage[15] is only touched
  // when head advances into it — which it doesn't here (head ends at 15,
  // that slot remains the sentinel gap).
  TEST_ASSERT_EQUAL_HEX8(0x5A, storage[15]);
  TEST_ASSERT_EQUAL_HEX8(0xC0, storage[10]);
  TEST_ASSERT_EQUAL_HEX8(0xC4, storage[14]);
}

// ---------------------------------------------------------------------------
// Read must not write past `n` bytes into the caller's output buffer
// ---------------------------------------------------------------------------

void test_read_does_not_overshoot_output_buffer() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {0x11, 0x22, 0x33};
  rb.write(data, 3);

  // Over-sized output buffer with a trailing sentinel region
  uint8_t out[16];
  std::memset(out, 0xAB, sizeof(out));

  const size_t r = rb.read(out, 3);  // ask for exactly 3 even though out is 16
  TEST_ASSERT_EQUAL(3u, r);

  // First 3 bytes are the data
  TEST_ASSERT_EQUAL_HEX8(0x11, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0x22, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0x33, out[2]);
  // Remaining bytes must be untouched
  for (size_t i = 3; i < sizeof(out); ++i) {
    TEST_ASSERT_EQUAL_HEX8(0xAB, out[i]);
  }
}

// ---------------------------------------------------------------------------
// used() and free_space() are purely observational — no side effects
// ---------------------------------------------------------------------------

void test_used_and_free_are_idempotent() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t data[20];
  fill_pattern(data, 20, 0x42);
  rb.write(data, 20);

  const size_t h0 = rb.head;
  const size_t t0 = rb.tail;

  // Call the observers many times
  for (int i = 0; i < 1000; ++i) {
    (void)rb.used();
    (void)rb.free_space();
  }
  // State must be completely unchanged
  TEST_ASSERT_EQUAL(h0, rb.head);
  TEST_ASSERT_EQUAL(t0, rb.tail);
  TEST_ASSERT_EQUAL(20u, rb.used());
  TEST_ASSERT_EQUAL(43u, rb.free_space());
}

// ---------------------------------------------------------------------------
// Head/tail indices always remain < capacity (wrap is modulo, not overflow)
// ---------------------------------------------------------------------------

void test_head_and_tail_stay_below_capacity() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t chunk[100];
  fill_pattern(chunk, 100, 0);

  // Hammer the buffer with varying sizes and verify invariants each time
  const size_t sizes[] = {1, 7, 31, 60, 63, 63, 17, 3, 50, 2};
  uint8_t rbuf[100];

  for (int round = 0; round < 500; ++round) {
    rb.write(chunk, sizes[round % 10]);
    TEST_ASSERT_TRUE(rb.head < rb.capacity);
    TEST_ASSERT_TRUE(rb.tail < rb.capacity);

    rb.read(rbuf, sizes[(round + 3) % 10]);
    TEST_ASSERT_TRUE(rb.head < rb.capacity);
    TEST_ASSERT_TRUE(rb.tail < rb.capacity);
  }
}

// ---------------------------------------------------------------------------
// Capacity-2 extended cycling: thousands of iterations keep 1-usable-byte
// invariant stable with no drift
// ---------------------------------------------------------------------------

static uint8_t backing_cap2_stress[2];

void test_capacity_two_ten_thousand_cycles() {
  RingBuffer rb = make_buf(backing_cap2_stress, 2);
  for (uint32_t i = 0; i < 10000; ++i) {
    const uint8_t v = static_cast<uint8_t>(i & 0xFF);
    TEST_ASSERT_EQUAL(1u, rb.write(&v, 1));
    TEST_ASSERT_EQUAL(0u, rb.free_space());
    TEST_ASSERT_EQUAL(1u, rb.used());

    uint8_t out = 0;
    TEST_ASSERT_EQUAL(1u, rb.read(&out, 1));
    TEST_ASSERT_EQUAL_HEX8(v, out);
    TEST_ASSERT_EQUAL(0u, rb.used());
    TEST_ASSERT_EQUAL(1u, rb.free_space());
  }
}

// ---------------------------------------------------------------------------
// head at position capacity-1: single write wraps head to 0
// ---------------------------------------------------------------------------

void test_head_at_last_slot_single_byte_wraps_to_zero() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance head to 7 (tail=7)
  uint8_t tmp[7];
  fill_pattern(tmp, 7, 0);
  rb.write(tmp, 7);
  rb.read(tmp, 7);
  TEST_ASSERT_EQUAL(7u, rb.head);
  TEST_ASSERT_EQUAL(7u, rb.tail);

  // Single-byte write: lands at storage[7], head moves 7 → 0 (mod 8)
  uint8_t b = 0x9F;
  TEST_ASSERT_EQUAL(1u, rb.write(&b, 1));
  TEST_ASSERT_EQUAL(0u, rb.head);
  TEST_ASSERT_EQUAL(7u, rb.tail);

  uint8_t out = 0;
  TEST_ASSERT_EQUAL(1u, rb.read(&out, 1));
  TEST_ASSERT_EQUAL_HEX8(0x9F, out);
  TEST_ASSERT_EQUAL(0u, rb.head);
  TEST_ASSERT_EQUAL(0u, rb.tail);
}

// ---------------------------------------------------------------------------
// Write exactly free_space+1 bytes → returns free_space; leaves buffer full
// ---------------------------------------------------------------------------

void test_write_one_more_than_free_returns_free_space() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Prime so free_space becomes 30
  uint8_t prime[33];
  fill_pattern(prime, 33, 0x20);
  rb.write(prime, 33);
  TEST_ASSERT_EQUAL(30u, rb.free_space());

  // Write free_space+1 = 31 bytes → exactly 30 fit
  uint8_t data[31];
  fill_pattern(data, 31, 0x70);
  TEST_ASSERT_EQUAL(30u, rb.write(data, 31));
  TEST_ASSERT_EQUAL(0u, rb.free_space());
  TEST_ASSERT_EQUAL(63u, rb.used());

  // Drain and verify FIFO order + integrity
  uint8_t out[63];
  TEST_ASSERT_EQUAL(63u, rb.read(out, 63));
  TEST_ASSERT_EQUAL_MEMORY(prime, out, 33);
  TEST_ASSERT_EQUAL_MEMORY(data, out + 33, 30);  // only first 30 of data[]
}

// ---------------------------------------------------------------------------
// Read used+1 bytes → returns used
// ---------------------------------------------------------------------------

void test_read_one_more_than_used_returns_used() {
  RingBuffer rb = make_buf(backing_small, 8);
  uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  rb.write(data, 5);
  TEST_ASSERT_EQUAL(5u, rb.used());

  uint8_t out[6];
  std::memset(out, 0xEE, sizeof(out));
  TEST_ASSERT_EQUAL(5u, rb.read(out, 6));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 5);
  TEST_ASSERT_EQUAL_HEX8(0xEE, out[5]);  // last slot untouched
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Write-then-read of equal length returns head==tail (empty-state canonical)
// ---------------------------------------------------------------------------

void test_equal_write_read_restores_head_tail_equal() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t data[30];
  fill_pattern(data, 30, 0x88);

  for (int cycle = 0; cycle < 20; ++cycle) {
    rb.write(data, 30);
    uint8_t out[30];
    rb.read(out, 30);
    TEST_ASSERT_EQUAL(rb.head, rb.tail);
    TEST_ASSERT_EQUAL(0u, rb.used());
  }
}

// ---------------------------------------------------------------------------
// Reset on an already-empty buffer is a no-op
// ---------------------------------------------------------------------------

void test_reset_on_empty_buffer_is_noop() {
  RingBuffer rb = make_buf(backing_small, 8);
  TEST_ASSERT_EQUAL(0u, rb.used());
  rb.reset();
  TEST_ASSERT_EQUAL(0u, rb.used());
  TEST_ASSERT_EQUAL(7u, rb.free_space());
  TEST_ASSERT_EQUAL(0u, rb.head);
  TEST_ASSERT_EQUAL(0u, rb.tail);

  // Still fully usable after the no-op reset
  uint8_t data[7];
  fill_pattern(data, 7, 0x55);
  TEST_ASSERT_EQUAL(7u, rb.write(data, 7));
}

// ---------------------------------------------------------------------------
// Randomised stress against std::deque reference model — catches subtle bugs
// ---------------------------------------------------------------------------

#include <deque>

// Simple deterministic PRNG so the test is reproducible without <random>.
struct Lcg {
  uint32_t state;
  explicit Lcg(uint32_t seed) : state(seed) {}
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
};

void test_randomised_vs_deque_reference() {
  RingBuffer rb = make_buf(backing_large, 1024);
  std::deque<uint8_t> model;
  const size_t usable = 1023;

  Lcg rng(0xC0FFEEu);
  uint8_t wbuf[300];
  uint8_t rbuf[300];
  uint8_t seq = 0;

  for (int step = 0; step < 20000; ++step) {
    const uint32_t r = rng.next();
    const size_t op = r & 0x3;                    // 0..3
    const size_t sz = 1 + ((r >> 2) & 0xFF);       // 1..256

    if (op == 0 || op == 1) {
      // Write path (2/4 probability)
      for (size_t i = 0; i < sz; ++i) wbuf[i] = seq++;
      const size_t before_free = usable - model.size();
      const size_t expected = (sz < before_free) ? sz : before_free;
      const size_t actual = rb.write(wbuf, sz);
      TEST_ASSERT_EQUAL(expected, actual);
      for (size_t i = 0; i < actual; ++i) model.push_back(wbuf[i]);
      // Roll seq back for un-written bytes
      if (actual < sz) seq = static_cast<uint8_t>(seq - (sz - actual));
    } else if (op == 2) {
      // Read path
      const size_t before_used = model.size();
      const size_t expected = (sz < before_used) ? sz : before_used;
      const size_t actual = rb.read(rbuf, sz);
      TEST_ASSERT_EQUAL(expected, actual);
      for (size_t i = 0; i < actual; ++i) {
        TEST_ASSERT_EQUAL_HEX8(model.front(), rbuf[i]);
        model.pop_front();
      }
    } else {
      // Rare reset — simulates job abort
      if ((r & 0xFF) == 0) {
        rb.reset();
        model.clear();
      } else {
        // Consume small chunks to keep things flowing
        const size_t drain = 1 + ((r >> 10) & 0x1F);
        const size_t before_used = model.size();
        const size_t expected = (drain < before_used) ? drain : before_used;
        const size_t actual = rb.read(rbuf, drain);
        TEST_ASSERT_EQUAL(expected, actual);
        for (size_t i = 0; i < actual; ++i) {
          TEST_ASSERT_EQUAL_HEX8(model.front(), rbuf[i]);
          model.pop_front();
        }
      }
    }

    // Check invariants every step
    TEST_ASSERT_EQUAL(model.size(), rb.used());
    TEST_ASSERT_EQUAL(usable - model.size(), rb.free_space());
    TEST_ASSERT_TRUE(rb.head < rb.capacity);
    TEST_ASSERT_TRUE(rb.tail < rb.capacity);
  }
}

// ---------------------------------------------------------------------------
// Reassigning storage between RingBuffer instances (shared backing memory)
// ---------------------------------------------------------------------------

void test_two_instances_sharing_storage_sequentially() {
  // Safety check: the struct is plain data and stores no internal pointers
  // beyond `storage`.  Two sequential instances using the same backing array
  // should not interfere — the second one just re-interprets the memory.
  uint8_t shared[32];

  {
    RingBuffer rb = make_buf(shared, 32);
    uint8_t data[10];
    fill_pattern(data, 10, 0x11);
    rb.write(data, 10);
    uint8_t out[10];
    rb.read(out, 10);
    TEST_ASSERT_EQUAL_MEMORY(data, out, 10);
  }

  // Fresh RingBuffer on the same bytes — starts empty even though storage
  // still holds the prior content.
  RingBuffer rb2 = make_buf(shared, 32);
  TEST_ASSERT_EQUAL(0u, rb2.used());
  uint8_t data2[20];
  fill_pattern(data2, 20, 0x77);
  rb2.write(data2, 20);
  uint8_t out2[20];
  TEST_ASSERT_EQUAL(20u, rb2.read(out2, 20));
  TEST_ASSERT_EQUAL_MEMORY(data2, out2, 20);
}

// ---------------------------------------------------------------------------
// After every write/read, used() + free_space() invariant holds (long run)
// ---------------------------------------------------------------------------

void test_invariant_after_every_operation_long_run() {
  RingBuffer rb = make_buf(backing_large, 1024);
  const size_t usable = 1023;
  uint8_t buf[300];
  fill_pattern(buf, 300, 0xAA);

  const size_t sizes[] = {1, 2, 5, 10, 50, 100, 200, 300, 7, 63};
  size_t idx = 0;

  for (int op = 0; op < 5000; ++op) {
    if (op % 2 == 0) {
      rb.write(buf, sizes[idx++ % 10]);
    } else {
      rb.read(buf, sizes[idx++ % 10]);
    }
    TEST_ASSERT_EQUAL(usable, rb.used() + rb.free_space());
  }
}

// ---------------------------------------------------------------------------
// Wrap during the second memcpy path: verify first and second halves are
// both placed at the correct storage addresses
// ---------------------------------------------------------------------------

void test_wrap_split_placement_verified_in_storage() {
  // Reach into storage to confirm the two halves of a wrapping write landed
  // at the expected byte positions.  This guards against a regression where
  // the second memcpy starts at the wrong offset or writes the wrong slice.
  uint8_t storage[16];
  std::memset(storage, 0x00, sizeof(storage));
  RingBuffer rb;
  rb.storage = storage;
  rb.capacity = 16;
  rb.head = 13;
  rb.tail = 13;

  // Write 6 bytes: 3 at positions 13..15, then wrap to positions 0..2
  uint8_t data[] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
  TEST_ASSERT_EQUAL(6u, rb.write(data, 6));

  // Verify precise byte placement in raw storage
  TEST_ASSERT_EQUAL_HEX8(0xA0, storage[13]);
  TEST_ASSERT_EQUAL_HEX8(0xA1, storage[14]);
  TEST_ASSERT_EQUAL_HEX8(0xA2, storage[15]);
  TEST_ASSERT_EQUAL_HEX8(0xA3, storage[0]);
  TEST_ASSERT_EQUAL_HEX8(0xA4, storage[1]);
  TEST_ASSERT_EQUAL_HEX8(0xA5, storage[2]);
  // Untouched region sanity check
  TEST_ASSERT_EQUAL_HEX8(0x00, storage[3]);
  TEST_ASSERT_EQUAL_HEX8(0x00, storage[12]);

  // head should now be at 3
  TEST_ASSERT_EQUAL(3u, rb.head);
  TEST_ASSERT_EQUAL(13u, rb.tail);
  TEST_ASSERT_EQUAL(6u, rb.used());
}

// ---------------------------------------------------------------------------
// Wrap during the second memcpy on read: verify source bytes are consumed
// from the correct storage positions
// ---------------------------------------------------------------------------

void test_wrap_split_read_sources_from_correct_positions() {
  uint8_t storage[16];
  // Prime storage with a recognisable pattern at every position
  for (uint8_t i = 0; i < 16; ++i) storage[i] = 0x10 + i;

  RingBuffer rb;
  rb.storage = storage;
  rb.capacity = 16;
  rb.tail = 13;  // start reading from position 13
  rb.head = 3;   // data runs 13,14,15,0,1,2

  TEST_ASSERT_EQUAL(6u, rb.used());

  uint8_t out[6];
  TEST_ASSERT_EQUAL(6u, rb.read(out, 6));
  TEST_ASSERT_EQUAL_HEX8(0x1D, out[0]);  // storage[13]
  TEST_ASSERT_EQUAL_HEX8(0x1E, out[1]);  // storage[14]
  TEST_ASSERT_EQUAL_HEX8(0x1F, out[2]);  // storage[15]
  TEST_ASSERT_EQUAL_HEX8(0x10, out[3]);  // storage[0]
  TEST_ASSERT_EQUAL_HEX8(0x11, out[4]);  // storage[1]
  TEST_ASSERT_EQUAL_HEX8(0x12, out[5]);  // storage[2]

  TEST_ASSERT_EQUAL(3u, rb.tail);
  TEST_ASSERT_EQUAL(3u, rb.head);
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Write/read pattern that repeatedly brings head or tail back to exactly 0
// (wrap-to-origin boundary)
// ---------------------------------------------------------------------------

void test_repeated_wrap_to_origin() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t chunk[64];

  // Each full-fill + full-drain cycle advances head and tail by exactly 63
  // (not 64), so after a few cycles they return near, but never stay at, 0.
  // This exercises the modulo wrap at non-aligned boundaries.
  uint8_t seq = 0;
  for (int cycle = 0; cycle < 100; ++cycle) {
    for (size_t i = 0; i < 63; ++i) chunk[i] = seq++;
    TEST_ASSERT_EQUAL(63u, rb.write(chunk, 63));
    TEST_ASSERT_EQUAL(63u, rb.used());
    TEST_ASSERT_EQUAL(0u, rb.free_space());

    uint8_t out[63];
    TEST_ASSERT_EQUAL(63u, rb.read(out, 63));
    // Verify the exact sequence we wrote came back in order
    uint8_t expect = static_cast<uint8_t>(seq - 63);
    for (size_t i = 0; i < 63; ++i) {
      TEST_ASSERT_EQUAL_HEX8(expect, out[i]);
      expect++;
    }
    TEST_ASSERT_EQUAL(rb.head, rb.tail);  // empty → both equal
  }
}

// ---------------------------------------------------------------------------
// reset() preserves storage/capacity pointers (only head/tail change)
// ---------------------------------------------------------------------------

void test_reset_preserves_storage_and_capacity() {
  RingBuffer rb = make_buf(backing_medium, 64);
  uint8_t data[40];
  fill_pattern(data, 40, 0x42);
  rb.write(data, 40);

  uint8_t *before_storage = rb.storage;
  const size_t before_capacity = rb.capacity;

  rb.reset();
  TEST_ASSERT_EQUAL_PTR(before_storage, rb.storage);
  TEST_ASSERT_EQUAL(before_capacity, rb.capacity);
  TEST_ASSERT_EQUAL(0u, rb.head);
  TEST_ASSERT_EQUAL(0u, rb.tail);
}

// ---------------------------------------------------------------------------
// Read at end-of-capacity exact boundary when tail == capacity - 1
// ---------------------------------------------------------------------------

void test_read_starts_at_last_slot_and_wraps() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Advance tail to 7
  uint8_t tmp[7];
  fill_pattern(tmp, 7, 0);
  rb.write(tmp, 7);
  rb.read(tmp, 7);
  TEST_ASSERT_EQUAL(7u, rb.tail);

  // Write 4 bytes starting at position 7 (wrap: 7, 0, 1, 2)
  uint8_t data[] = {0x51, 0x52, 0x53, 0x54};
  TEST_ASSERT_EQUAL(4u, rb.write(data, 4));

  // Read them — first byte comes from storage[7], then storage[0..2]
  uint8_t out[4];
  TEST_ASSERT_EQUAL(4u, rb.read(out, 4));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 4);
  TEST_ASSERT_EQUAL(3u, rb.tail);
  TEST_ASSERT_EQUAL(3u, rb.head);
}

// ---------------------------------------------------------------------------
// Large buffer: 3 KB streaming with mismatched chunk sizes
// ---------------------------------------------------------------------------

static uint8_t backing_large_stress[3072];

void test_large_buffer_long_streaming_integrity() {
  RingBuffer rb = make_buf(backing_large_stress, 3072);
  const size_t target = 500000;
  const size_t write_chunk = 311;  // prime
  const size_t read_chunk = 257;   // prime

  uint8_t wbuf[311];
  uint8_t rbuf[257];
  uint8_t write_seq = 0;
  uint8_t read_seq = 0;
  size_t total_written = 0;
  size_t total_read = 0;

  while (total_written < target) {
    for (size_t i = 0; i < write_chunk; ++i) wbuf[i] = write_seq++;
    const size_t w = rb.write(wbuf, write_chunk);
    total_written += w;
    if (w < write_chunk)
      write_seq = static_cast<uint8_t>(write_seq - (write_chunk - w));

    const size_t r = rb.read(rbuf, read_chunk);
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(read_seq, rbuf[i]);
      read_seq++;
    }
    total_read += r;
  }

  // Drain remainder
  while (rb.used() > 0) {
    const size_t r = rb.read(rbuf, read_chunk);
    for (size_t i = 0; i < r; ++i) {
      TEST_ASSERT_EQUAL_HEX8(read_seq, rbuf[i]);
      read_seq++;
    }
    total_read += r;
  }
  TEST_ASSERT_EQUAL(total_written, total_read);
}

// ---------------------------------------------------------------------------
// free_space never exceeds capacity-1 at any (head, tail) configuration
// ---------------------------------------------------------------------------

void test_free_space_never_exceeds_usable() {
  for (size_t cap = 2; cap <= 32; ++cap) {
    uint8_t buf[32];
    RingBuffer rb;
    rb.storage = buf;
    rb.capacity = cap;
    for (size_t h = 0; h < cap; ++h) {
      for (size_t t = 0; t < cap; ++t) {
        rb.head = h;
        rb.tail = t;
        TEST_ASSERT_TRUE(rb.free_space() <= cap - 1);
        TEST_ASSERT_TRUE(rb.used() <= cap - 1);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Write a chunk whose internal boundary aligns exactly on the wrap
// ---------------------------------------------------------------------------

void test_write_with_to_end_equal_to_n_minus_one() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance head to position 63 (last slot): write 63, read 63
  uint8_t tmp[63];
  fill_pattern(tmp, 63, 0);
  rb.write(tmp, 63);
  rb.read(tmp, 63);
  TEST_ASSERT_EQUAL(63u, rb.head);
  TEST_ASSERT_EQUAL(63u, rb.tail);

  // Write 10 bytes: first memcpy covers 1 byte (to_end = 64-63 = 1),
  // second memcpy covers remaining 9 bytes at positions 0..8
  uint8_t data[10];
  fill_pattern(data, 10, 0xF0);
  TEST_ASSERT_EQUAL(10u, rb.write(data, 10));

  uint8_t out[10];
  TEST_ASSERT_EQUAL(10u, rb.read(out, 10));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 10);
}

// ---------------------------------------------------------------------------
// Read a chunk whose internal boundary aligns exactly on the wrap
// ---------------------------------------------------------------------------

void test_read_with_to_end_equal_to_n_minus_one() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance tail to position 63
  uint8_t tmp[63];
  fill_pattern(tmp, 63, 0);
  rb.write(tmp, 63);
  rb.read(tmp, 63);

  // Write 10 bytes (1 at pos 63, then 9 wrapping to 0..8)
  uint8_t data[10];
  fill_pattern(data, 10, 0xE0);
  rb.write(data, 10);

  // Read all 10 — first memcpy reads 1 byte from pos 63, second reads 9 from 0
  uint8_t out[10];
  TEST_ASSERT_EQUAL(10u, rb.read(out, 10));
  TEST_ASSERT_EQUAL_MEMORY(data, out, 10);
  TEST_ASSERT_EQUAL(9u, rb.tail);
  TEST_ASSERT_EQUAL(9u, rb.head);
}

// ---------------------------------------------------------------------------
// Fill buffer, read partial, reset — subsequent writes start clean at index 0
// ---------------------------------------------------------------------------

void test_reset_mid_drain_normalises_indices() {
  RingBuffer rb = make_buf(backing_small, 8);

  // Fill, then partial drain
  uint8_t data[7];
  fill_pattern(data, 7, 0x30);
  rb.write(data, 7);
  uint8_t drain[3];
  rb.read(drain, 3);
  TEST_ASSERT_EQUAL(4u, rb.used());
  TEST_ASSERT_TRUE(rb.tail != 0 || rb.head != 0);  // indices are advanced

  rb.reset();
  TEST_ASSERT_EQUAL(0u, rb.head);
  TEST_ASSERT_EQUAL(0u, rb.tail);
  TEST_ASSERT_EQUAL(0u, rb.used());

  // New write starts at storage[0]
  uint8_t storage_before_write_at_0 = rb.storage[0];
  (void)storage_before_write_at_0;
  uint8_t fresh[] = {0xF1, 0xF2};
  TEST_ASSERT_EQUAL(2u, rb.write(fresh, 2));
  TEST_ASSERT_EQUAL_HEX8(0xF1, rb.storage[0]);
  TEST_ASSERT_EQUAL_HEX8(0xF2, rb.storage[1]);
}

// ---------------------------------------------------------------------------
// Consecutive write calls accumulate correctly across a wrap boundary
// ---------------------------------------------------------------------------

void test_consecutive_writes_accumulate_across_wrap() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance head and tail to 58
  uint8_t tmp[58];
  fill_pattern(tmp, 58, 0);
  rb.write(tmp, 58);
  rb.read(tmp, 58);

  // Three small writes that collectively straddle the wrap
  uint8_t a[] = {0x01, 0x02, 0x03};    // lands 58,59,60
  uint8_t b[] = {0x04, 0x05, 0x06};    // lands 61,62,63
  uint8_t c[] = {0x07, 0x08, 0x09};    // wraps to 0,1,2
  rb.write(a, 3);
  rb.write(b, 3);
  rb.write(c, 3);
  TEST_ASSERT_EQUAL(9u, rb.used());

  uint8_t out[9];
  TEST_ASSERT_EQUAL(9u, rb.read(out, 9));
  for (uint8_t i = 0; i < 9; ++i) {
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(i + 1), out[i]);
  }
}

// ---------------------------------------------------------------------------
// Consecutive read calls drain correctly across a wrap boundary
// ---------------------------------------------------------------------------

void test_consecutive_reads_drain_across_wrap() {
  RingBuffer rb = make_buf(backing_medium, 64);

  // Advance tail to 58
  uint8_t tmp[58];
  fill_pattern(tmp, 58, 0);
  rb.write(tmp, 58);
  rb.read(tmp, 58);

  // Write 15 bytes (6 at end, 9 wrapping)
  uint8_t data[15];
  fill_pattern(data, 15, 0xC0);
  rb.write(data, 15);

  // Drain in three separate reads
  uint8_t r1[5], r2[5], r3[5];
  TEST_ASSERT_EQUAL(5u, rb.read(r1, 5));
  TEST_ASSERT_EQUAL(5u, rb.read(r2, 5));
  TEST_ASSERT_EQUAL(5u, rb.read(r3, 5));
  TEST_ASSERT_EQUAL_MEMORY(data, r1, 5);
  TEST_ASSERT_EQUAL_MEMORY(data + 5, r2, 5);
  TEST_ASSERT_EQUAL_MEMORY(data + 10, r3, 5);
  TEST_ASSERT_EQUAL(0u, rb.used());
}

// ---------------------------------------------------------------------------
// Writing into a capacity-1 buffer (no usable slot) always fails — even when
// called multiple times (no state drift)
// ---------------------------------------------------------------------------

void test_capacity_one_many_writes_no_drift() {
  uint8_t tiny[1];
  RingBuffer rb;
  rb.storage = tiny;
  rb.capacity = 1;
  rb.head = 0;
  rb.tail = 0;

  for (int i = 0; i < 1000; ++i) {
    uint8_t data = static_cast<uint8_t>(i);
    TEST_ASSERT_EQUAL(0u, rb.write(&data, 1));
    TEST_ASSERT_EQUAL(0u, rb.used());
    TEST_ASSERT_EQUAL(0u, rb.free_space());
    TEST_ASSERT_EQUAL(0u, rb.head);
    TEST_ASSERT_EQUAL(0u, rb.tail);
  }
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

  // Prime-chunk concurrent-style stress
  RUN_TEST(test_prime_chunk_stress);

  // Exact capacity-minus-one boundary write/read
  RUN_TEST(test_exact_capacity_minus_one_write_read);

  // Partial write leaves exactly zero free space
  RUN_TEST(test_partial_write_leaves_zero_free);

  // Multiple sequential partial writes fill to capacity
  RUN_TEST(test_multiple_partial_writes_to_capacity);

  // Read more than buffer has ever held
  RUN_TEST(test_read_more_than_ever_written);

  // Reset at various head/tail positions verifies head==0, tail==0
  RUN_TEST(test_reset_at_various_positions);

  // Capacity==2 many single-byte cycles
  RUN_TEST(test_capacity_two_many_cycles);

  // Firmware drain pattern (variable TCP writes, fixed 1024-byte reads)
  RUN_TEST(test_firmware_drain_pattern);

  // Write exactly free_space bytes to fill completely
  RUN_TEST(test_write_exact_free_space);

  // Slow producer, fast consumer
  RUN_TEST(test_slow_producer_fast_consumer);

  // Rapid fill-to-full / drain-to-empty cycles
  RUN_TEST(test_rapid_fill_drain_full_cycles);

  // Write fills exactly to buffer end (to_end boundary)
  RUN_TEST(test_write_fills_exactly_to_end);

  // Read consumes exactly to buffer end
  RUN_TEST(test_read_consumes_exactly_to_end);

  // Prefill then interleaved streaming
  RUN_TEST(test_prefill_then_interleaved_streaming);

  // Reset then immediate full write (rapid job cycling)
  RUN_TEST(test_reset_then_immediate_full_write);

  // Write SIZE_MAX doesn't crash
  RUN_TEST(test_write_size_max_no_crash);

  // used() when head < tail (wrapped state)
  RUN_TEST(test_used_when_head_less_than_tail);

  // Batch accumulation then large drain
  RUN_TEST(test_batch_accumulation_then_drain);

  // free_space degenerate capacities
  RUN_TEST(test_free_space_degenerate);

  // Reset does not leak stale data (bytes gone even if storage retains them)
  RUN_TEST(test_reset_does_not_leak_stale_data);

  // Write capacity bytes — writes only capacity-1 (sentinel)
  RUN_TEST(test_write_capacity_bytes_writes_capacity_minus_one);

  // 1000 fill-drain cycles — storage not corrupted
  RUN_TEST(test_many_cycles_no_storage_corruption);

  // Sentinel invariant at every head position
  RUN_TEST(test_sentinel_at_every_position);

  // 1023 single-byte writes, single bulk drain preserves order
  RUN_TEST(test_single_byte_writes_bulk_drain_keeps_order);

  // Tail wraps to 0 on non-wrapping read (lands at capacity boundary)
  RUN_TEST(test_tail_wraps_to_zero_on_non_wrapping_read);

  // Producer faster than consumer — backpressure verification
  RUN_TEST(test_producer_faster_than_consumer_backpressure);

  // Large read straddling wrap boundary
  RUN_TEST(test_large_read_straddling_wrap);

  // Write+reset+write discards pre-reset data
  RUN_TEST(test_write_reset_write_discards_pre_reset_data);

  // Second memcpy path correctness (wrap during read)
  RUN_TEST(test_second_memcpy_correctness);

  // used() exhaustive (head, tail) combos for small buffers
  RUN_TEST(test_used_invariant_exhaustive_small);

  // Full buffer 1-byte cycling preserves FIFO integrity
  RUN_TEST(test_full_buffer_one_byte_cycling_integrity);

  // Write after wrap state — subsequent writes place correctly
  RUN_TEST(test_write_after_wrap_state);

  // --- Additional corner-case coverage ---

  // Null + zero-length combos
  RUN_TEST(test_write_null_and_zero_length);
  RUN_TEST(test_read_null_and_zero_length);
  RUN_TEST(test_write_with_null_storage_and_null_data);
  RUN_TEST(test_read_with_null_storage_and_null_data);

  // Capacity 3 (smallest non-trivial)
  RUN_TEST(test_capacity_three_edge_behavior);

  // Non-destructive write/read
  RUN_TEST(test_partial_write_does_not_touch_past_free_region);
  RUN_TEST(test_read_does_not_overshoot_output_buffer);

  // Observer idempotency
  RUN_TEST(test_used_and_free_are_idempotent);

  // Head/tail bounds
  RUN_TEST(test_head_and_tail_stay_below_capacity);

  // Capacity-2 stress
  RUN_TEST(test_capacity_two_ten_thousand_cycles);

  // Head at last slot
  RUN_TEST(test_head_at_last_slot_single_byte_wraps_to_zero);

  // free_space ± 1 boundary
  RUN_TEST(test_write_one_more_than_free_returns_free_space);
  RUN_TEST(test_read_one_more_than_used_returns_used);

  // Empty-state canonical
  RUN_TEST(test_equal_write_read_restores_head_tail_equal);

  // No-op reset
  RUN_TEST(test_reset_on_empty_buffer_is_noop);

  // Randomised stress vs. std::deque reference
  RUN_TEST(test_randomised_vs_deque_reference);

  // Sequential reuse of same storage
  RUN_TEST(test_two_instances_sharing_storage_sequentially);

  // Invariant during long random ops
  RUN_TEST(test_invariant_after_every_operation_long_run);

  // Verify exact byte placement across wrap
  RUN_TEST(test_wrap_split_placement_verified_in_storage);
  RUN_TEST(test_wrap_split_read_sources_from_correct_positions);

  // Repeated wrap-to-origin cycles
  RUN_TEST(test_repeated_wrap_to_origin);

  // reset() preserves struct pointers
  RUN_TEST(test_reset_preserves_storage_and_capacity);

  // Read starting at last slot
  RUN_TEST(test_read_starts_at_last_slot_and_wraps);

  // Long streaming on 3KB buffer
  RUN_TEST(test_large_buffer_long_streaming_integrity);

  // free_space never exceeds capacity-1
  RUN_TEST(test_free_space_never_exceeds_usable);

  // to_end equal to n-1 (small first memcpy)
  RUN_TEST(test_write_with_to_end_equal_to_n_minus_one);
  RUN_TEST(test_read_with_to_end_equal_to_n_minus_one);

  // Reset mid-drain normalises indices
  RUN_TEST(test_reset_mid_drain_normalises_indices);

  // Consecutive writes / reads across wrap
  RUN_TEST(test_consecutive_writes_accumulate_across_wrap);
  RUN_TEST(test_consecutive_reads_drain_across_wrap);

  // Capacity-1 can never hold data
  RUN_TEST(test_capacity_one_many_writes_no_drift);

  return UNITY_END();
}
