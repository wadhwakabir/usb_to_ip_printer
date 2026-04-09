#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Lock-free-safe ring buffer core.  All index math lives here; synchronisation
// (mutexes, semaphores) is the caller's responsibility.
//
// The buffer uses a one-byte sentinel so that head==tail always means empty,
// giving (capacity - 1) usable bytes.

struct RingBuffer {
  uint8_t *storage = nullptr;
  size_t capacity = 0;  // raw storage size; usable = capacity - 1
  size_t head = 0;
  size_t tail = 0;

  size_t used() const {
    return (head >= tail) ? (head - tail) : (capacity - tail + head);
  }

  size_t free_space() const { return capacity - 1 - used(); }

  // Write up to `len` bytes.  Returns the number actually written (may be less
  // if the buffer is full or nearly full).
  size_t write(const uint8_t *data, size_t len) {
    const size_t avail = free_space();
    const size_t n = (len < avail) ? len : avail;
    if (n > 0) {
      const size_t to_end = capacity - head;
      const size_t first = (n < to_end) ? n : to_end;
      std::memcpy(storage + head, data, first);
      if (n > first) {
        std::memcpy(storage, data + first, n - first);
      }
      head = (head + n) % capacity;
    }
    return n;
  }

  // Read up to `max_len` bytes.  Returns the number actually read.
  size_t read(uint8_t *data, size_t max_len) {
    const size_t avail = used();
    const size_t n = (max_len < avail) ? max_len : avail;
    if (n > 0) {
      const size_t to_end = capacity - tail;
      const size_t first = (n < to_end) ? n : to_end;
      std::memcpy(data, storage + tail, first);
      if (n > first) {
        std::memcpy(data + first, storage, n - first);
      }
      tail = (tail + n) % capacity;
    }
    return n;
  }

  void reset() {
    head = 0;
    tail = 0;
  }
};
