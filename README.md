# USB-to-IP Printer Bridge (ESP32-S3)

Turn any USB printer into a network printer. An ESP32-S3 accepts print jobs
over Wi-Fi on TCP port 9100 (RAW/AppSocket protocol) and forwards the data to
a USB printer via the host port. Printer responses (PJL status, etc.) are read
from the USB bulk IN endpoint and relayed back to the TCP client, providing
full **bidirectional communication**.

Your computer's printer driver renders the job; the ESP32 simply shuttles
bytes in both directions -- no printer-specific firmware needed.

> **Canon G1010 is used as the reference printer throughout.** The bridge works
> with any USB printer that exposes bulk endpoints (virtually all of them).

## Features

- **Bidirectional USB** -- print data flows to the printer via bulk OUT;
  printer responses return via bulk IN and are forwarded to the TCP client
- **WiFi STA with AP fallback** -- connects to an existing network or creates
  its own access point when no credentials are configured
- **mDNS advertisement** -- `_printer._tcp` and `_pdl-datastream._tcp` service
  records, reachable as `<hostname>.local`
- **512 KB PSRAM ring buffer** -- read-ahead buffer with a 64 KB pre-fill gate
  to smooth WiFi jitter before USB output begins
- **Dedicated FreeRTOS tasks** -- drain task (core 0) for USB OUT transfers,
  receive task (core 1) for USB IN polling
- **TCP debug console** on port 2323 (telnet) -- inspect live status, recover
  from USB faults, and reboot remotely without physical access
- **NeoPixel RGB LED** status indication with a state machine
- **Robust error handling** -- endpoint recovery (halt/flush/clear), transfer
  timeouts, auto-reconnect on WiFi loss
- **Native unit tests** for the ring buffer (runs on host, no hardware needed)
- **Docker-based test runner** for CI or isolated builds

## Data Flow

```
Client --> TCP:9100 --> WiFi --> ring buffer (512KB PSRAM) --> drain task --> USB bulk OUT --> Printer
Printer --> USB bulk IN --> recv task --> TCP:9100 --> Client
```

Step by step:

1. The printer driver on your computer renders the document into the printer's
   native format.
2. The driver sends the rendered bytes to the ESP32 on TCP port 9100.
3. The ESP32 TCP server reads chunks into the PSRAM-backed ring buffer.
4. A pre-fill gate accumulates ~64 KB before the drain task starts sending to
   USB, so the printer gets a steady stream despite WiFi jitter.
5. The drain task (core 0) reads from the ring buffer and writes to the USB
   bulk OUT endpoint.
6. The receive task (core 1) polls the printer's bulk IN endpoint and relays
   any responses back to the TCP client.

## Hardware

- **ESP32-S3-DevKitC-1 N16R8** -- 16 MB flash, 8 MB octal PSRAM
- Any USB printer (Canon G1010, HP, Epson, Brother, etc.)
- USB cable from the ESP32-S3 host port to the printer

A custom board definition lives in `boards/esp32-s3-n16r8.json`, and the
partition table (`partitions/default_16MB.csv`) provides dual OTA slots.

## Quick Start

**Zero-to-printing in two commands** (requires `make` and either PlatformIO
or Docker):

```bash
git clone <repo-url> && cd usb_to_ip_printer
make flash                         # compile + upload to connected ESP32-S3
make monitor                       # watch it boot
```

No config files to edit. Every unit flashes identical. After flash, the device
brings up its own Wi-Fi (`esp32-printer` / `printprint`) with a captive
portal — the end user enters their home SSID/password from a phone browser,
and the device stores them in NVS. See `setup.html` for the end-user guide you
can print or ship alongside the hardware.

`make help` shows all available targets. `make doctor` checks your environment
and lists available USB serial ports.

## Setup (detailed)

### 1. Wi-Fi configuration

None at build time. Credentials are provisioned by the end user at first boot:

1. Flash the firmware and power the device. The LED blinks purple.
2. On a phone, join the Wi-Fi network `esp32-printer` (password `printprint`).
3. A captive-portal "sign in to network" page pops up (or open
   `http://192.168.4.1` manually).
4. Pick the home SSID, enter the password, tap **Save & reboot**.
5. The device reboots, joins the home network, and LED goes solid green
   (or orange if no printer is attached yet).

Credentials live in NVS. On later boots the device reconnects automatically.
If the STA join fails for 2 minutes (router down, moved to a new home,
password rotated), the device falls back into the captive portal — no serial
cable required.

To re-provision manually: `nc <device-ip> 2323` → `forget-wifi`.

### 2. Wire the USB printer

The ESP32-S3 must be wired in **USB host mode** to the printer -- this is
separate from the UART/USB bridge used for serial monitor and flashing.

- Connect USB D+ and D- from the ESP32-S3 host-capable pins to the printer
- The printer side must see valid VBUS power
- The printer should be self-powered or externally powered

## Building and Flashing

### Make workflow (recommended)

| Command          | Purpose                                                      |
|------------------|--------------------------------------------------------------|
| `make build`     | Compile firmware                                             |
| `make flash`     | Compile + flash to connected ESP32-S3                        |
| `make monitor`   | Open serial monitor (115200 baud)                            |
| `make dev`       | Flash then open monitor (typical edit-flash-debug loop)      |
| `make test`      | Run native unit tests                                        |
| `make ports`     | List attached USB serial devices                             |
| `make doctor`    | Check environment for required tools                         |
| `make clean`     | Remove build artefacts                                       |

The Makefile auto-detects whether to use a local PlatformIO install or Docker.
Force Docker with `make build USE_DOCKER=1`. Flashing always uses a local
PlatformIO install because Docker cannot reliably forward USB on macOS.

### Docker-only (no local PlatformIO required)

| Command              | Purpose                                              |
|----------------------|------------------------------------------------------|
| `make docker-image`  | Build both Docker images (one time, ~5 min cold)     |
| `make docker-build`  | Compile firmware inside the container                |
| `make docker-test`   | Run native tests inside the container                |

The build image caches the ESP32-S3 toolchain inside the image, so subsequent
`docker-build` runs are fast. The generated `firmware.bin` lands on the host
at `.pio/build/esp32-s3-n16r8/firmware.bin`. Flash it with any tool that can
talk to the ESP32-S3 bootloader (e.g. `esptool.py`), or run `make flash` once
PlatformIO is installed locally.

### Manual / advanced

Plain PlatformIO still works:

```bash
pio run                          # build
pio run -t upload                # flash
pio device monitor               # serial monitor
pio test -e native-tests         # tests
```

Note the printed IP address from the serial monitor, or connect to the
setup AP (`esp32-printer` / `printprint`) if no credentials are stored yet.

### LED pin

The onboard RGB LED defaults to `GPIO48` (ESP32-S3-DevKitC-1). If your board
uses a different pin, change the build flag in `platformio.ini`:

```ini
build_flags =
  -DRGB_LED_PIN=38
```

## Testing

The ring buffer has a comprehensive test suite (100+ tests covering wrap
boundaries, backpressure, data integrity, sentinel invariants, a randomized
stress test against a `std::deque` reference model, and more) that runs
natively on the host -- no ESP32 hardware required.

```bash
make test                  # local or docker, whichever is available
make docker-test           # always run in docker
```

## Usage

### Printing

The ESP32 speaks standard RAW/AppSocket on port 9100. Add it as a network
printer and install the driver for your USB printer model.

#### Windows

1. **Settings > Bluetooth & devices > Printers & scanners > Add device > Add
   manually**.
2. Select **Add a printer using a TCP/IP address or hostname**.
3. Enter the ESP32's IP address (or `esp32-printer.local`).
4. Protocol: **Raw**, Port: **9100**.
5. Install the driver for your printer model (e.g. Canon G1010).

#### macOS

1. **System Settings > Printers & Scanners > +** (IP tab).
2. Address: ESP32's IP (or `esp32-printer.local`).
3. Protocol: **HP Jetdirect - Socket**.
4. Select the appropriate driver or PPD.

#### Linux (CUPS)

```bash
lpadmin -p "USB-Print-Bridge" \
  -v "socket://192.168.1.42:9100" \
  -P /path/to/your-printer.ppd \
  -E
```

A Canon G1010 PPD file (`CanonIJG1000series.ppd.gz`) is included in the repo
for CUPS integration.

### Quick test with netcat

```bash
# Send a raw file
nc <esp32-ip> 9100 < test-print.bin

# Text smoke test
printf "test page\n" | nc <esp32-ip> 9100
```

### Debug console

Connect to port 2323 with telnet or netcat:

```bash
nc <esp32-ip> 2323
```

Available commands:

| Command       | Description                                                      |
|---------------|------------------------------------------------------------------|
| `status`      | Full bridge status (network, USB, job, buffer, heap) in one dump |
| `net`         | Mode/SSID/IP/RSSI summary                                        |
| `usb`         | USB counters, endpoints, last error                              |
| `job`         | Current job byte/chunk counters                                  |
| `buf`         | Ring buffer usage and drain state                                |
| `heap`        | Free heap, uptime, boot count, reset reason                      |
| `clear-error` | Clear a stuck USB drain error after reattaching the printer      |
| `forget-wifi` | Erase saved Wi-Fi credentials and reboot into the setup portal   |
| `reboot`      | Restart the ESP32 (drops the session)                            |
| `help`        | List all commands                                                |
| `close`       | Close the debug session                                          |

**Recovering from a USB fault.** If the printer loses USB mid-job, subsequent
jobs are rejected until the error is cleared — this is intentional so silent
drops don't happen. After power-cycling or re-seating the printer cable:

```
$ nc <esp32-ip> 2323
esp32-print> usb          # confirm device=yes ready=yes
esp32-print> clear-error
drain_error cleared.
esp32-print> close
```

If the device is truly wedged (e.g. USB host task hung), `reboot` restarts the
firmware without needing physical access.

## Troubleshooting

**Job appears to complete but nothing prints.** Check `usb` on the debug
console. If `faulted=yes` or `ready=no`, the USB backend gave up mid-job. Power
cycle the printer, then run `clear-error` and reprint. The firmware intentionally
rejects new jobs after a drain error to avoid silently discarding data.

**"No USB printer attached" / stuck on orange LED.** The device sees no USB
printer. Verify the printer is powered, the cable goes to the ESP32's **host**
USB port (not the UART/programming port), and the printer shows up in `usb` as
`device=yes`. Some printers need 30-60 s after power-on to enumerate.

**Prints start then stall mid-page.** Usually WiFi jitter combined with a large
job exceeding the 512 KB buffer. Check `buf` — if `used=524287` persistently,
the drain side is keeping up but the sender is filling faster than USB can
empty. Move the ESP32 closer to the AP, or wire Ethernet via an ESP32-S3
variant with RMII pins.

**Device unreachable after a reboot.** The saved STA credentials may be
wrong or the network gone. After ~2 minutes of failed auto-reconnect the
firmware brings up the setup AP (`esp32-printer` / `printprint`) with the
captive portal at `http://192.168.4.1`. Re-enter credentials there, or
connect and run `nc 192.168.4.1 2323` for the debug console. The IP is also
logged on boot via `make monitor`.

**Can't find the debug console.** It's on TCP 2323, separate from the print
port 9100. Use `nc <ip> 2323` or `telnet <ip> 2323`.

## Architecture

```
+-------------------------------------------------------------+
|                     ESP32-S3 Firmware                        |
|                                                              |
|  +-----------+    +------------------+    +---------------+  |
|  | WiFi      |    | Ring Buffer      |    | USB Host      |  |
|  | TCP Server|--->| (512KB PSRAM)    |--->| Printer       |  |
|  | port 9100 |    |                  |    | Bridge        |  |
|  +-----------+    | mutex-guarded    |    +-------+-------+  |
|                   | semaphore-       |            |          |
|  +-----------+    | signaled         |    +-------+-------+  |
|  | Debug     |    +------------------+    | USB bulk IN   |  |
|  | Console   |                            | recv task     |--+--> TCP:9100
|  | port 2323 |                            +---------------+  |
|  +-----------+    +---------------+                          |
|                   | NeoPixel LED  |                          |
|                   | State Machine |                          |
|                   +---------------+                          |
|                                                              |
|  FreeRTOS Tasks:                                             |
|   - main loop  : TCP accept, read, LED, heartbeat           |
|   - buf_drain  : ring buffer -> USB OUT          (core 0)   |
|   - usb_recv   : USB IN -> TCP client            (core 1)   |
|   - usb_lib    : USB host library event loop                |
|   - usb_client : device attach/detach handling              |
+-------------------------------------------------------------+
```

### Ports

| Port | Protocol | Purpose                    |
|------|----------|----------------------------|
| 9100 | TCP      | RAW print data (bidirectional) |
| 2323 | TCP      | Debug console (telnet)     |

### Network modes

On boot the firmware loads STA credentials from NVS. If they're present it
tries to join that network; otherwise (or if the join fails for 2 minutes) it
brings up its own access point and a captive portal for first-time or
post-move provisioning.

| Setting        | Value                                     |
|----------------|-------------------------------------------|
| Setup AP SSID  | `esp32-printer`                           |
| Setup AP pass  | `printprint`                              |
| Setup URL      | `http://192.168.4.1` (or captive pop-up)  |
| Hostname (STA) | `esp32-printer.local`                     |

## LED Status Guide

| Color / Pattern       | State              | Meaning                      |
|-----------------------|--------------------|------------------------------|
| White (solid)         | Booting            | Firmware initializing        |
| Blue (blinking)       | WifiConnecting     | Connecting to WiFi station   |
| Purple (blinking)     | AccessPointReady   | AP mode active, waiting      |
| Orange (blinking)     | WaitingForPrinter  | WiFi up, no USB printer yet  |
| Green (solid)         | Ready              | Ready for print jobs         |
| Cyan (solid)          | ClientConnected    | TCP client connected         |
| Yellow (fast blink)   | Printing           | Receiving/printing data      |
| Green (fast blink)    | JobComplete        | Job finished successfully    |
| Red (fast blink)      | Error              | USB or system error          |

## Project Structure

```
boards/
  esp32-s3-n16r8.json        # Custom board definition (16MB flash, 8MB PSRAM)
include/
  ring_buffer.h               # Header-only lock-free ring buffer
  usb_printer_bridge.h        # USB host printer bridge API
src/
  main.cpp                    # WiFi, captive portal, TCP servers, LED, FreeRTOS tasks
  usb_printer_bridge.cpp      # USB host driver (ESP-IDF USB Host Library)
test/
  test_ring_buffer/
    test_main.cpp             # Native unit tests for ring buffer (100+ tests)
partitions/
  default_16MB.csv            # Partition table with dual OTA slots
Dockerfile                    # Containerized native test runner
docker-compose.yml            # Docker Compose service for tests
CanonIJG1000series.ppd.gz     # Canon G1010 PPD for CUPS
platformio.ini                # Build configuration (esp32-s3-n16r8 + native-tests)
setup.html                    # End-user setup guide to print or ship with device
```
