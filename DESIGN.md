# Design Document: USB-to-IP Print Bridge

This document explains the entire firmware architecture in plain language,
drawing Python analogies wherever possible. If you can read Python, you can
understand what this C++ firmware is doing.

---

## Table of Contents

1. [Big Picture](#1-big-picture)
2. [Python vs C++ Embedded — Key Differences](#2-python-vs-c-embedded--key-differences)
3. [File Map](#3-file-map)
4. [Boot Sequence (setup)](#4-boot-sequence-setup)
5. [Main Loop (loop)](#5-main-loop-loop)
6. [State Machine](#6-state-machine)
7. [Ring Buffer — The Core Data Structure](#7-ring-buffer--the-core-data-structure)
8. [The Print Buffer Wrapper](#8-the-print-buffer-wrapper)
9. [Print Job Lifecycle](#9-print-job-lifecycle)
10. [USB Printer Bridge](#10-usb-printer-bridge)
11. [Concurrency Model (FreeRTOS Tasks)](#11-concurrency-model-freertos-tasks)
12. [Network Layer](#12-network-layer)
13. [Debug Console](#13-debug-console)
14. [LED Feedback](#14-led-feedback)
15. [Memory Layout](#15-memory-layout)
16. [Configuration](#16-configuration)
17. [Error Handling Philosophy](#17-error-handling-philosophy)
18. [Constants Reference](#18-constants-reference)

---

## 1. Big Picture

```
┌──────────────┐       Wi-Fi        ┌────────────────┐        USB         ┌─────────────┐
│  Computer    │  TCP port 9100     │   ESP32-S3     │  Bulk OUT endpoint │ USB Printer  │
│  (has the    │ ──────────────────>│   (this code)  │ ──────────────────>│ (any model)  │
│   driver)    │  raw bytes         │                │  same raw bytes    │              │
└──────────────┘                    └────────────────┘                    └─────────────┘
```

**In one sentence:** The computer's printer driver renders a document into raw
bytes, sends them over Wi-Fi to the ESP32, which buffers them and forwards them
over USB to the physical printer.

Think of the ESP32 as a **transparent pipe** — it doesn't understand the print
data. It doesn't know if you're printing a photo or a spreadsheet. It just
moves bytes from TCP socket to USB endpoint.

### Python analogy

```python
# This is conceptually what the entire firmware does:
import socket, usb.core

tcp = socket.socket()
tcp.bind(("0.0.0.0", 9100))
tcp.listen(1)

printer = usb.core.find(bInterfaceClass=0x07)  # USB printer class
endpoint = printer[0][(0,0)][0]                 # bulk OUT endpoint

while True:
    client, addr = tcp.accept()
    while chunk := client.recv(4096):
        endpoint.write(chunk)
    client.close()
```

The real firmware is ~1000 lines longer because it handles:
- Wi-Fi connection/reconnection and fallback access point
- A 512 KB ring buffer to decouple network speed from USB speed
- Concurrent tasks (so receiving and sending happen in parallel)
- USB hot-plug (printer can be connected/disconnected at any time)
- LED status feedback, debug console, error recovery, timeouts

---

## 2. Python vs C++ Embedded — Key Differences

If you're coming from Python, these are the mental model shifts:

| Concept | Python | This Firmware (C++ on ESP32) |
|---|---|---|
| **Entry point** | `if __name__ == "__main__":` | `setup()` runs once, then `loop()` runs forever |
| **Concurrency** | `asyncio` or `threading` | FreeRTOS tasks (real OS-level threads on two CPU cores) |
| **Memory** | Garbage collected, unlimited-ish | 512 KB RAM + 8 MB PSRAM, manual allocation, no GC |
| **Buffers** | `bytearray` grows as needed | Fixed-size ring buffer, pre-allocated at boot |
| **Networking** | `socket` module | Arduino `WiFiServer` / `WiFiClient` (thin wrapper over lwIP) |
| **USB** | `pyusb` / `libusb` | ESP-IDF USB Host library (register-level driver) |
| **Locks** | `threading.Lock()` | FreeRTOS `SemaphoreHandle_t` (mutex) |
| **Signals** | `threading.Event()` | FreeRTOS binary semaphore |
| **Types** | `int`, `bytes` | `uint8_t` (1 byte), `uint16_t` (2 bytes), `uint32_t` (4 bytes), `size_t` |
| **Strings** | `str` (unicode) | `const char *` (C strings, null-terminated byte arrays) |
| **Enums** | `enum.Enum` | `enum class` (same idea, but integers underneath) |
| **Structs** | `@dataclass` | `struct` (a class where everything is public by default) |
| **Namespaces** | modules / packages | `namespace { }` (anonymous = file-private, like `_private` convention) |
| **`constexpr`** | module-level `CONSTANT = 42` | Compile-time constant; the value is baked into the binary |
| **`volatile`** | N/A | "Don't optimize reads to this variable away — another thread changes it" |
| **`static`** | class variable | Here: a global that persists across function calls |

### The `setup()` / `loop()` Pattern

Arduino programs have two entry points:

```cpp
void setup() {   // Runs once at power-on. Like __init__() for the whole device.
    // Initialize hardware, connect Wi-Fi, start servers
}

void loop() {    // Runs forever after setup(). Like `while True:` in Python.
    // Check for clients, read data, update LED, etc.
}
```

There is no `main()` — the Arduino framework provides it and calls
`setup()` then `while(true) { loop(); }` internally.

---

## 3. File Map

```
include/
  network_config.h        # Your Wi-Fi SSID/password (#define constants)
  ring_buffer.h            # Pure data structure — the circular byte buffer
  usb_printer_bridge.h     # Public API for the USB subsystem

src/
  main.cpp                 # Everything else: Wi-Fi, TCP, LED, jobs, debug
  usb_printer_bridge.cpp   # USB host driver implementation

test/
  test_ring_buffer/        # Unit tests for the ring buffer

platformio.ini             # Build config (like pyproject.toml / setup.cfg)
partitions/
  default_16MB.csv         # Flash memory partition layout
```

### How the files relate

```
main.cpp
  ├── uses ring_buffer.h        (the buffer data structure)
  ├── uses usb_printer_bridge.h (calls begin/is_ready/send_raw/etc.)
  └── uses network_config.h     (Wi-Fi credentials)

usb_printer_bridge.cpp
  └── implements usb_printer_bridge.h
```

There are only **2 source files**. `main.cpp` is the orchestrator.
`usb_printer_bridge.cpp` is a self-contained USB driver that exposes a
simple API.

---

## 4. Boot Sequence (setup)

Here's what happens when the ESP32 powers on, in order:

```
Power on
  │
  ├─ 1. Start serial at 115200 baud (for debug output)
  ├─ 2. Initialize RGB LED
  ├─ 3. Try connecting to Wi-Fi (station mode)
  │     ├─ Success → use the home network
  │     └─ Fail/empty SSID → start fallback access point
  ├─ 4. Start mDNS (so you can use esp32-print-bridge.local)
  ├─ 5. Start USB host subsystem
  │     ├─ Creates usb_lib daemon task
  │     └─ Creates usb_client task
  ├─ 6. Allocate 512KB ring buffer (PSRAM preferred, heap fallback)
  ├─ 7. Create buf_drain FreeRTOS task (pinned to CPU core 0)
  ├─ 8. Start TCP print server on port 9100
  ├─ 9. Start TCP debug server on port 2323
  └─ 10. Set state to Ready (or WaitingForPrinter)
```

### Python equivalent (conceptual)

```python
def setup():
    serial = Serial(115200)
    led = NeoPixel(pin=48)

    if not wifi.connect(SSID, PASSWORD, timeout=15):
        wifi.start_ap("ESP32-Print-Bridge", "printbridge")

    mdns.advertise("esp32-print-bridge", services=["printer/tcp:9100"])

    usb_bridge.begin()                  # start USB host driver
    ring_buffer = RingBuffer(512*1024)  # allocate buffer in PSRAM

    # Start background thread for draining buffer to USB
    Thread(target=drain_buffer_to_usb, daemon=True).start()

    tcp_server = TCPServer(("0.0.0.0", 9100))
    debug_server = TCPServer(("0.0.0.0", 2323))
```

---

## 5. Main Loop (loop)

The main loop runs on the Arduino task (core 1) and does everything in a
single-threaded, poll-based style — like a Python `asyncio` event loop but
without `await`:

```
loop() — runs every 1-5ms
  │
  ├─ updateLed()               # Set LED color based on current state
  ├─ processPrintStream()      # Read TCP data into ring buffer
  ├─ pollForClients()          # Accept new / detect disconnected clients
  ├─ pollDebugServer()         # Handle debug console (skipped while printing)
  ├─ restoreReadyStateIfNeeded()  # Auto-recover from transient states
  ├─ syncIdleStateWithPrinter()   # Update state when USB printer connects
  ├─ checkWifiConnection()     # Detect Wi-Fi drops, update state
  ├─ logHeartbeat()            # Print status every 5s (serial monitor)
  └─ delay(1 or 5 ms)         # Yield CPU; 1ms during printing, 5ms idle
```

### Why this order matters

`processPrintStream()` runs **before** `pollForClients()`. This is critical:

```
processPrintStream()   →  reads available TCP data while socket is alive
pollForClients()       →  calls connected() which can poison the socket

If you reverse the order, connected() marks the socket as dead, and then
processPrintStream() sees available()=0, losing the final data chunk.
```

This is like a Python gotcha where `socket.recv()` after the peer closes still
returns buffered data, but if you check `is_connected()` first, the library
might drain the buffer internally.

---

## 6. State Machine

The firmware tracks a single global state that drives LED color and behavior:

```
                          ┌───────────┐
                          │  Booting  │
                          └─────┬─────┘
                                │
                     ┌──────────┴──────────┐
                     │                     │
              ┌──────▼──────┐       ┌──────▼────────┐
              │   WiFi      │       │  Access Point │
              │ Connecting  │       │     Ready     │
              └──────┬──────┘       └───────┬───────┘
                     │                      │
                     ▼                      │
              ┌──────────────┐              │
              │ Waiting For  │◄─────────────┘
              │   Printer    │   (no USB printer yet)
              └──────┬───────┘
                     │ USB printer detected
                     ▼
              ┌──────────────┐
        ┌────►│    Ready     │◄──────────────────────────┐
        │     └──────┬───────┘                           │
        │            │ TCP client connects               │
        │            ▼                                   │
        │     ┌──────────────┐                           │
        │     │   Client     │                           │
        │     │  Connected   │                           │
        │     └──────┬───────┘                           │
        │            │ first data received               │
        │            ▼                                   │
        │     ┌──────────────┐                           │
        │     │  Printing    │                           │
        │     └──────┬───────┘                           │
        │            │ job finishes                      │
        │            ▼                                   │
        │     ┌──────────────┐     after 1.2s            │
        │     │ Job Complete ├───────────────────────────►│
        │     └──────────────┘                           │
        │                                                │
        │     ┌──────────────┐     after 1.5s            │
        └─────┤    Error     ├───────────────────────────┘
              └──────────────┘   (if fault cleared)
```

### Python equivalent

```python
class State(Enum):
    BOOTING = auto()
    WIFI_CONNECTING = auto()
    ACCESS_POINT_READY = auto()
    WAITING_FOR_PRINTER = auto()
    READY = auto()
    CLIENT_CONNECTED = auto()
    PRINTING = auto()
    JOB_COMPLETE = auto()
    ERROR = auto()

current_state = State.BOOTING

def set_state(new_state):
    global current_state
    if current_state == new_state:
        return  # no-op, prevents log spam
    print(f"[STATE] {current_state.name} -> {new_state.name}")
    current_state = new_state
```

---

## 7. Ring Buffer — The Core Data Structure

**File:** `include/ring_buffer.h`

This is a circular (ring) buffer — a fixed-size array that wraps around. Data
is written at `head` and read from `tail`. When `head` reaches the end of the
array, it wraps back to position 0.

### Why a ring buffer?

Wi-Fi data arrives in bursts (network jitter). USB printers consume data at a
steady rate. Without a buffer, the printer would starve between network bursts
and the print quality would suffer (banding, pauses).

The ring buffer absorbs bursts and feeds the printer a smooth stream:

```
Wi-Fi bursts:    ████░░░░████░░████░░░░░░████████
                        ↓ ring buffer ↓
USB steady:      ██████████████████████████████████
```

### How it works visually

```
capacity = 8 (indices 0-7), usable = 7 (one sentinel slot)

Empty (head == tail):
  [ _ _ _ _ _ _ _ _ ]
    ↑
   head=0
   tail=0

After writing [A, B, C]:
  [ A B C _ _ _ _ _ ]
        ↑ ↑
       head=3
    tail=0

After reading 2 bytes (returns [A, B]):
  [ _ _ C _ _ _ _ _ ]
        ↑ ↑
       head=3
       tail=2

Wrap-around — tail at 6, write [X, Y, Z, W]:
  [ Z W _ _ _ _ X Y ]
      ↑         ↑
    head=2    tail=6
    (wrapped!)
```

### Python equivalent

```python
class RingBuffer:
    def __init__(self, capacity: int):
        self.storage = bytearray(capacity)
        self.capacity = capacity
        self.head = 0   # next write position
        self.tail = 0   # next read position

    def used(self) -> int:
        if self.head >= self.tail:
            return self.head - self.tail
        return self.capacity - self.tail + self.head

    def free_space(self) -> int:
        if self.capacity < 2:
            return 0
        return self.capacity - 1 - self.used()

    def write(self, data: bytes) -> int:
        """Write as many bytes as will fit. Returns count written."""
        if not data or self.capacity < 2:
            return 0
        avail = self.free_space()
        n = min(len(data), avail)
        for i in range(n):
            self.storage[self.head] = data[i]
            self.head = (self.head + 1) % self.capacity
        return n

    def read(self, max_len: int) -> bytes:
        """Read up to max_len bytes. Returns what's available."""
        avail = self.used()
        n = min(max_len, avail)
        result = bytearray(n)
        for i in range(n):
            result[i] = self.storage[self.tail]
            self.tail = (self.tail + 1) % self.capacity
        return bytes(result)

    def reset(self):
        self.head = 0
        self.tail = 0
```

### Why capacity - 1?

The buffer sacrifices one slot as a **sentinel**. If we used all slots, a full
buffer (`head == tail` after wrapping) would look identical to an empty buffer
(`head == tail` at start). The sentinel avoids this ambiguity:

- `head == tail` always means **empty**
- `(head + 1) % capacity == tail` means **full**

### The actual C++ code uses `memcpy` for performance

Instead of copying byte-by-byte, the C++ version splits the write into at most
two `memcpy` calls — one for the chunk before the wrap point, one for the chunk
after. This is much faster than a Python-style loop.

```
Write 6 bytes starting at position 5 in an 8-slot buffer:

Chunk 1: positions 5, 6, 7  (3 bytes — "to_end" of array)
Chunk 2: positions 0, 1, 2  (3 bytes — wrapped portion)
```

---

## 8. The Print Buffer Wrapper

**File:** `src/main.cpp` (the `PrintRingBuffer` struct and buffer* functions)

The raw `RingBuffer` is not thread-safe. The wrapper adds:

| Component | Python Equivalent | Purpose |
|---|---|---|
| `mutex` | `threading.Lock()` | Only one thread can read/write the buffer at a time |
| `data_ready` | `threading.Event()` | Wakes up the drain task when new data arrives |
| `drain_task` | `Thread(target=...)` | Background thread that sends buffer data to USB |
| `drain_error` | `bool` flag | Set `True` if USB write fails |
| `job_active` | `bool` flag | Is there a print job in progress? |
| `job_generation` | Counter (like a version number) | Distinguishes current job from previous ones |
| `prefill_done` | `bool` flag | Has the pre-fill watermark been reached? |

### The pre-fill gate

Before starting USB output, the drain task waits until the buffer has ~64 KB
of data (or 2 seconds elapse, whichever comes first). This prevents the
printer from receiving a few bytes, pausing, receiving a few more bytes, etc.

```python
# Conceptual pre-fill logic
if buffer.used() < PREFILL_BYTES and elapsed < PREFILL_TIMEOUT:
    continue  # keep waiting, don't send to USB yet

# Once past the gate, drain continuously
while job_active:
    chunk = buffer.read(1024)
    if chunk:
        usb.send(chunk)
```

---

## 9. Print Job Lifecycle

A print job goes through these phases:

```
1. CLIENT CONNECTS
   │
   │  beginJob()
   │  - Reset the ring buffer (clear any stale data)
   │  - Increment job_generation (invalidate stale drain errors)
   │  - Mark job as active
   │  - Record start time
   │
   ▼
2. DATA FLOWS
   │
   │  processPrintStream() — runs every loop iteration
   │  ┌─────────────────────────────────────────────────────┐
   │  │  while tcp_client.available() > 0:                  │
   │  │      check buffer free space                        │
   │  │      if buffer full: break (let drain task catch up)│
   │  │      read chunk from TCP into tcpReadBuf            │
   │  │      write chunk into ring buffer                   │
   │  │      update byte/chunk counters                     │
   │  └─────────────────────────────────────────────────────┘
   │
   │  Meanwhile, on another CPU core:
   │  printBufferDrainTask() — runs continuously
   │  ┌─────────────────────────────────────────────────────┐
   │  │  wait for data_ready semaphore                      │
   │  │  [pre-fill gate: wait for 64KB or 2s timeout]      │
   │  │  while job_active and not drain_error:              │
   │  │      read 1KB chunk from ring buffer                │
   │  │      send to USB printer (usb_printer_bridge)       │
   │  └─────────────────────────────────────────────────────┘
   │
   ▼
3. JOB ENDS (one of these triggers)
   │
   │  a) Client disconnects normally → finishJob("client disconnected")
   │  b) Idle timeout (30s no data)  → finishJob("idle timeout")
   │  c) Probe timeout (5s, no data ever received) → finishJob("probe timeout")
   │  d) USB drain error             → finishJob("buffer drain failed")
   │
   │  finishJob()
   │  - Flush remaining buffer to USB (wait up to 30s)
   │  - Mark job inactive
   │  - Log statistics (bytes, chunks, duration)
   │  - Set state to JobComplete (or Error)
   │
   ▼
4. RECOVERY
   │
   │  After 1.2 seconds in JobComplete, state returns to Ready
   │  After 1.5 seconds in Error (if fault cleared), state returns to Ready
```

### Timeout design

Two different timeouts handle two scenarios:

```python
# A client that connects but never sends data (port scanner, health check)
PROBE_TIMEOUT = 5_000   # 5 seconds

# A real print job where data stops mid-stream
IDLE_TIMEOUT  = 30_000  # 30 seconds (printers can be slow)
```

### Job generation counter

Imagine this race condition:
1. Job A fails on USB
2. The drain task sets `drain_error = True`
3. Job A finishes and resets the buffer
4. Job B starts
5. The drain task sees a stale error from Job A

The `job_generation` counter prevents this. The drain task captures the
generation number at the start and only sets `drain_error` if the generation
still matches. If a new job started, the stale error is silently discarded.

```python
# Conceptual
gen = job_generation  # capture at loop start

if usb_send_failed:
    if job_generation == gen:   # still the same job?
        drain_error = True      # yes — report the error
    # else: ignore — the job that caused this is already gone
```

---

## 10. USB Printer Bridge

**Files:** `include/usb_printer_bridge.h` (API) and `src/usb_printer_bridge.cpp`
(implementation)

### Public API

The USB subsystem exposes a deliberately simple API — think of it as a Python
module:

```python
# usb_printer_bridge API (conceptual Python)

def begin() -> bool:
    """Initialize USB host hardware. Call once at boot. Returns success."""

def is_ready() -> bool:
    """Is a printer connected AND its bulk OUT endpoint claimed?"""

def has_device() -> bool:
    """Is any USB device physically connected?"""

def is_faulted() -> bool:
    """Did the last USB transfer fail?"""

def send_raw(data: bytes) -> bool:
    """Send raw bytes to the printer. Blocks until complete or error."""

def get_status() -> UsbPrinterBridgeStatus:
    """Return a snapshot of all USB state (for debug/monitoring)."""

def last_error() -> str:
    """Human-readable description of the last error."""
```

### How USB printer detection works

When you plug a USB printer into the ESP32:

```
1. USB hardware detects voltage on VBUS → interrupt
2. usb_lib daemon processes the event
3. usb_client task receives NEW_DEV callback
4. handle_new_device() opens the device and reads its descriptor
   - Logs: VID, PID, manufacturer, product name
5. inspect_interfaces() scans all USB interfaces:
   - Looking for: bInterfaceClass == 0x07 (USB Printer class)
   - Falls back to: any interface with a Bulk OUT endpoint
6. Claims the interface and allocates a transfer buffer
7. Sets printer_ready = true
```

### Python analogy for USB enumeration

```python
# What inspect_interfaces() does, conceptually:

import usb.core, usb.util

device = usb.core.find()  # whatever just got plugged in

# Look through all interfaces
for config in device:
    for interface in config:
        print(f"Interface {interface.bInterfaceNumber}: "
              f"class=0x{interface.bInterfaceClass:02x}")

        if interface.bInterfaceClass == 0x07:  # Printer class
            for ep in interface:
                if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT:
                    if usb.util.endpoint_type(ep.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK:
                        print(f"  Found Bulk OUT endpoint: 0x{ep.bEndpointAddress:02x}")
                        # This is our target endpoint
```

### USB transfer flow

Sending data to the printer is synchronous within a mutex:

```
send_raw(data, length)
  │
  ├─ Acquire send_mutex (only one send at a time)
  ├─ Split data into chunks of 1024 bytes (transfer buffer size)
  │   └─ For each chunk:
  │       ├─ Copy chunk into USB transfer buffer
  │       ├─ Set endpoint address, callback, length
  │       ├─ Submit transfer to USB hardware
  │       ├─ Wait for transfer_done semaphore (up to 15s)
  │       ├─ Check completion status
  │       │   ├─ Success → increment total_forwarded_bytes
  │       │   └─ Failure → set backend_faulted, recover endpoint
  │       └─ Next chunk (or return false on failure)
  └─ Release send_mutex
```

### Endpoint recovery

If a USB transfer fails (timeout, stall, device error), the firmware attempts
to recover without a full device reset:

```python
def recover_endpoint(reason):
    """Halt, flush, clear — the USB error recovery sequence."""
    usb_host_endpoint_halt(endpoint)   # tell device to stop
    usb_host_endpoint_flush(endpoint)  # discard queued transfers
    usb_host_endpoint_clear(endpoint)  # reset endpoint state
```

### Hot-plug handling

USB devices can be connected and disconnected at any time. Two events:

```
NEW_DEV event:
  → handle_new_device() → inspect_interfaces() → printer_ready = true

DEV_GONE event:
  → Acquire send_mutex (wait for any in-flight transfer to finish)
  → Release USB interface
  → Close device handle
  → Reset all state to "no printer"
  → Release send_mutex
```

The `send_mutex` acquisition during DEV_GONE ensures we don't yank the device
away while a transfer is in progress.

---

## 11. Concurrency Model (FreeRTOS Tasks)

The ESP32-S3 has **two CPU cores**. The firmware uses 4 FreeRTOS tasks:

```
Core 1 (Arduino default)          Core 0
┌──────────────────────┐          ┌────────────────────────┐
│ Main Task            │          │ buf_drain              │
│ (setup + loop)       │          │ (printBufferDrainTask) │
│                      │          │                        │
│ - TCP accept/read    │  ring    │ - Read from buffer     │
│ - Write to buffer ──►│  buffer  │◄─ Send to USB printer  │
│ - LED update         │          │                        │
│ - Debug console      │          │ Priority: 10           │
│ - State machine      │          │ Stack: 4KB             │
│                      │          └────────────────────────┘
│ Priority: 1          │
│ Stack: 8KB           │          ┌────────────────────────┐
└──────────────────────┘          │ usb_lib + usb_client   │
                                  │ (USB host tasks)       │
                                  │                        │
                                  │ - Handle USB events    │
                                  │ - Device attach/detach │
                                  │                        │
                                  │ Priority: 19-20        │
                                  │ Stack: 4-6KB           │
                                  └────────────────────────┘
```

### Why two cores?

If TCP reading and USB writing happened on the same thread:

```
Thread:  [read TCP]  →  [write USB ... slow ...]  →  [read TCP]  →  [write USB ...]
                         ↑                                            ↑
                    TCP data arriving here is buffered by OS
                    but if the OS buffer fills, data is dropped
```

With separate threads:

```
Core 1:  [read TCP → buffer] [read TCP → buffer] [read TCP → buffer] ...
Core 0:  [buffer → write USB] [buffer → write USB] [buffer → write USB] ...
          ↑ runs in parallel ↑
```

### Synchronization primitives

```python
# In Python terms:

# 1. Mutex — only one thread touches the ring buffer at a time
buffer_lock = threading.Lock()

# 2. Binary semaphore — "there's new data, wake up"
data_ready = threading.Event()

# 3. Send mutex — only one USB transfer at a time
send_lock = threading.Lock()

# 4. Transfer done semaphore — "USB hardware finished the transfer"
transfer_done = threading.Event()
```

### volatile keyword

Several fields in `PrintRingBuffer` are marked `volatile`:

```cpp
volatile bool drain_error = false;
volatile bool job_active = false;
volatile uint32_t job_generation = 0;
```

In Python, you never need this because the GIL prevents certain optimizations.
In C++, the compiler might optimize `while (!drain_error)` into
`if (!drain_error) while(true)` — caching the value in a CPU register. The
`volatile` keyword tells the compiler: "re-read this from memory every time,
another thread might have changed it."

---

## 12. Network Layer

### Wi-Fi modes

```python
# Conceptual
def connect_wifi():
    if WIFI_STA_SSID:
        wifi.mode = STATION
        wifi.connect(WIFI_STA_SSID, WIFI_STA_PASSWORD, timeout=15)
        if wifi.connected:
            wifi.auto_reconnect = True  # handles drops automatically
            return True

    # Fallback: become an access point
    wifi.mode = ACCESS_POINT
    wifi.start_ap("ESP32-Print-Bridge", "printbridge")
    return True
```

### TCP servers

Two TCP servers run simultaneously:

| Port | Purpose | Max Clients |
|------|---------|-------------|
| 9100 | RAW print data (AppSocket protocol) | 1 (extras get "BUSY" response) |
| 2323 | Debug console (telnet-like) | 1 |

Only **one print client** at a time. If a second client connects while a job is
active, it receives "BUSY" and is disconnected. This matches how physical
printers work — they serve one job at a time.

### mDNS

The firmware advertises two mDNS services:

```
_printer._tcp     — generic network printer discovery
_pdl-datastream._tcp  — AppSocket/RAW protocol discovery
```

This lets operating systems auto-discover the printer on the local network.

---

## 13. Debug Console

Connecting to port 2323 gives you a simple text command interface:

```bash
$ nc 192.168.1.42 2323
ESP32 Print Bridge Debug Console
Type 'help' for commands.
esp32-print> status
state=ready
mode=STA
ip=192.168.1.42
...
esp32-print> usb
ready=yes device=yes vid=04a9 pid=1827 out=0x02
forwarded=1048576 dropped=0 failed=0
esp32-print> buf
used=0 capacity=524288 drain_error=no job_active=no
```

Available commands:

| Command | Shows |
|---------|-------|
| `status` | Full system status (Wi-Fi, USB, job, buffer, heap) |
| `usb` | USB device info and transfer counters |
| `job` | Current print job stats |
| `buf` | Ring buffer fill level and error state |
| `heap` | Free memory, uptime, boot count, reset reason |
| `help` | Command list |
| `close` / `exit` / `quit` | Disconnect |

The debug console is **paused during printing** to avoid stealing CPU time from
the print path.

---

## 14. LED Feedback

The onboard RGB NeoPixel LED indicates the current state:

| State | Color | Pattern | Blink Rate |
|-------|-------|---------|------------|
| Booting | White | Solid | — |
| WiFi Connecting | Blue | Blink | 250ms |
| Access Point Ready | Purple | Blink | 700ms |
| Waiting for Printer | Orange | Blink | 400ms |
| Ready | Green | Solid | — |
| Client Connected | Cyan | Solid | — |
| Printing | Yellow | Fast blink | 150ms |
| Job Complete | Green | Fast blink | 120ms |
| Error | Red | Fast blink | 120ms |

The blinking is implemented without timers — just `millis() / rate % 2`:

```python
# Python equivalent of the blink logic
import time

def blink_color(r, g, b, rate_ms):
    """Returns (r,g,b) or (0,0,0) alternating at the given rate."""
    if (int(time.monotonic() * 1000) // rate_ms) % 2 == 0:
        return (r, g, b)
    return (0, 0, 0)
```

---

## 15. Memory Layout

The ESP32-S3 N16R8 has:

```
┌─────────────────────────────────────────┐
│ Internal SRAM (512 KB)                  │
│  ├─ FreeRTOS kernel + task stacks       │
│  ├─ Arduino WiFi/TCP buffers            │
│  ├─ USB host driver state               │
│  ├─ TCP read buffer (4 KB static)       │
│  ├─ Global variables                    │
│  └─ Heap (dynamic allocations)          │
│     └─ 64 KB reserved for runtime       │
│        (if PSRAM unavailable, 32KB      │
│         ring buffer allocated here)     │
├─────────────────────────────────────────┤
│ PSRAM (8 MB)                            │
│  └─ Ring buffer: 512 KB                 │
│     (preferred location)                │
├─────────────────────────────────────────┤
│ Flash (16 MB)                           │
│  ├─ Bootloader                          │
│  ├─ Partition table                     │
│  ├─ Application firmware                │
│  └─ NVS (non-volatile storage)          │
└─────────────────────────────────────────┘
```

### Why PSRAM for the ring buffer?

Internal SRAM is only 512 KB and must be shared with the Wi-Fi stack, TCP
buffers, FreeRTOS, and USB host driver. A 512 KB ring buffer wouldn't fit.
PSRAM is slower but has 8 MB of space, and the ring buffer access pattern
(sequential writes/reads) works well with PSRAM's characteristics.

If PSRAM is not available (some boards don't have it), the firmware falls back
to a 32 KB buffer from internal heap.

---

## 16. Configuration

All configuration is in `include/network_config.h` (gitignored):

```c
#define WIFI_STA_SSID     "YourWiFiSSID"      // home/office Wi-Fi
#define WIFI_STA_PASSWORD "YourWiFiPassword"
#define WIFI_AP_SSID      "ESP32-Print-Bridge" // fallback AP name
#define WIFI_AP_PASSWORD  "printbridge"        // fallback AP password
#define PRINTER_HOSTNAME  "esp32-print-bridge" // mDNS hostname
```

Build-time configuration in `platformio.ini`:

```ini
build_flags = -DRGB_LED_PIN=48   # Change to 38 if LED doesn't work
```

### Compile-time constants (in main.cpp)

These are `constexpr` values — hardcoded at compile time, not configurable
at runtime:

```
kRawPrintPort        = 9100       # Standard RAW print port
kDebugPort           = 2323       # Debug console port
kWifiConnectTimeoutMs= 15000     # 15s Wi-Fi connect timeout
kClientProbeTimeoutMs= 5000      # 5s for clients that connect but send nothing
kPrintIdleTimeoutMs  = 30000     # 30s idle timeout during a print job
kChunkBufferSize     = 4096      # TCP read chunk size
kDrainChunkSize      = 1024      # USB write chunk size
kPrintBufferCapacity = 524288    # 512KB ring buffer (PSRAM)
kHeapFallbackCapacity= 32768    # 32KB ring buffer (heap fallback)
kHeapReserveBytes    = 65536     # 64KB minimum free heap to keep
kBufferFlushTimeoutMs= 30000    # 30s max wait when flushing buffer at job end
kPrefillBytes        = 65536     # 64KB pre-fill before USB drain starts
kPrefillTimeoutMs    = 2000      # 2s max wait for pre-fill
```

---

## 17. Error Handling Philosophy

The firmware follows these principles:

### 1. Fail visible, not silent

Every error is logged to serial AND reflected in the LED state. If the USB
transfer fails, the LED turns red. If Wi-Fi drops, the LED shows blue blink.

### 2. Auto-recover when possible

- Wi-Fi drops → auto-reconnect is enabled, state recovers when reconnected
- USB transfer error → endpoint recovery (halt/flush/clear)
- USB device disconnect → state cleanly resets, ready for reconnect
- Transient error states → auto-clear after 1.5 seconds if the fault resolves

### 3. Don't lose print data

- Buffer checks free space **before** reading from TCP (never pull bytes you
  can't store)
- The residual drain loop in `pollForClients()` captures data that arrived
  between the last `processPrintStream()` call and the disconnect detection
- `flushPrintBuffer()` waits up to 30 seconds for the drain task to deliver
  remaining data to USB before declaring the job done
- `processPrintStream()` deliberately avoids calling `connected()` because
  it can poison the socket's internal state

### 4. Reject what you can't handle

- Second TCP client gets "BUSY" response and is disconnected
- If no USB printer is attached, incoming jobs fail immediately (not silently
  dropped)

---

## 18. Constants Reference

Quick lookup for the "magic numbers" in the code:

| Constant | Value | Why This Value |
|---|---|---|
| Port 9100 | Standard RAW/AppSocket | Every OS knows this as a raw print port |
| Port 2323 | Debug console | Arbitrary; avoids common ports |
| 4096 TCP chunk | `kChunkBufferSize` | Matches typical TCP segment size |
| 1024 USB chunk | `kDrainChunkSize` | Matches `kTransferBufferSize` in USB bridge |
| 512 KB buffer | `kPrintBufferCapacity` | Fits comfortably in 8 MB PSRAM |
| 64 KB prefill | `kPrefillBytes` | Enough to keep printer fed during Wi-Fi jitter |
| 32 KB fallback | `kHeapFallbackCapacity` | Largest safe allocation from 512 KB heap |
| 15s Wi-Fi timeout | `kWifiConnectTimeoutMs` | Typical home router association time |
| 5s probe timeout | `kClientProbeTimeoutMs` | Reject port scanners quickly |
| 30s idle timeout | `kPrintIdleTimeoutMs` | Printers can pause between pages |
| 30s flush timeout | `kBufferFlushTimeoutMs` | Large jobs may take time to drain |
| 2s prefill timeout | `kPrefillTimeoutMs` | Don't delay small jobs too long |
| 15s USB transfer timeout | `kTransferTimeoutTicks` | USB stall recovery threshold |
| GPIO 48 | `RGB_LED_PIN` | DevKitC-1 v1.1 onboard NeoPixel |
| 0x07 | `USB_CLASS_PRINTER` | USB-IF standard printer class code |
