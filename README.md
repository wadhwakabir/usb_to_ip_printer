# USB to IP Print Bridge (ESP32-S3)

Turn any USB printer into a network printer. An ESP32-S3 accepts RAW TCP print
jobs over Wi-Fi on port `9100` and forwards them to a USB printer connected to
its host port. Your computer's printer driver renders the job; the ESP32 simply
shuttles the bytes over USB — no printer-specific firmware needed.

> **Canon G1010 is used as an example throughout this README.** The bridge works
> with any USB printer that exposes a Bulk OUT endpoint (which covers virtually
> all USB printers).

## How It Works

```
┌──────────────┐          Wi-Fi / LAN          ┌──────────────┐         USB          ┌─────────────┐
│              │  RAW TCP port 9100             │              │  Bulk OUT endpoint   │             │
│   Computer   │ ──────────────────────────────>│   ESP32-S3   │ ───────────────────> │ USB Printer │
│  (driver     │  print data stream             │  Print Bridge│  raw data forwarded  │ (any model) │
│   renders)   │                                │              │                      │             │
└──────────────┘                                └──────────────┘                      └─────────────┘
       │                                               │
       │  1. OS printer driver renders                 │  3. Drain task sends buffered
       │     the document into RAW bytes               │     data to USB Bulk OUT endpoint
       │  2. Sends over TCP to ESP32 on port 9100      │  4. Printer receives and prints
       │                                               │
       ▼                                               ▼
  Printer added as                              512KB PSRAM ring buffer
  "RAW TCP/IP printer"                          decouples Wi-Fi jitter
  pointing at ESP32 IP                          from USB transfer rate
```

### Data Flow (Step by Step)

1. **Computer** — The printer driver (e.g. Canon G1010 driver on Windows)
   renders the document into the printer's native format.
2. **TCP send** — The driver sends the rendered bytes to the ESP32's IP address
   on port `9100` (standard RAW/AppSocket protocol).
3. **ESP32 Wi-Fi ingress** — A TCP server accepts the connection and reads
   chunks into a 512 KB PSRAM-backed ring buffer.
4. **Buffer drain task** — A dedicated FreeRTOS task on core 0 reads from the
   ring buffer and writes to the USB printer's Bulk OUT endpoint. A pre-fill
   gate accumulates ~64 KB before starting USB output so the printer gets a
   steady stream despite Wi-Fi jitter.
5. **USB printer** — Receives the raw byte stream and prints.

### Internal Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        ESP32-S3 Firmware                    │
│                                                             │
│  ┌─────────────┐    ┌──────────────────┐    ┌───────────┐  │
│  │  Wi-Fi      │    │  Ring Buffer     │    │  USB Host │  │
│  │  TCP Server ├───>│  (512KB PSRAM)   ├───>│  Printer  │  │
│  │  port 9100  │    │                  │    │  Bridge   │  │
│  └─────────────┘    │  mutex-guarded   │    └───────────┘  │
│                     │  semaphore-      │                    │
│  ┌─────────────┐    │  signaled        │    ┌───────────┐  │
│  │  Debug      │    └──────────────────┘    │  RGB LED  │  │
│  │  Console    │                            │  Status   │  │
│  │  port 2323  │                            │  Feedback │  │
│  └─────────────┘                            └───────────┘  │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  FreeRTOS Tasks                                     │    │
│  │  - main loop: TCP accept, read, LED, heartbeat      │    │
│  │  - buf_drain (core 0): ring buffer -> USB OUT       │    │
│  │  - usb_lib: USB host library event loop             │    │
│  │  - usb_client: device attach/detach handling        │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

## Hardware

- **ESP32-S3-DevKitC-1 N16R8** (16 MB flash, 8 MB octal PSRAM)
- Any USB printer (Canon G1010, HP, Epson, Brother, etc.)
- USB cable from ESP32-S3 host port to the printer

## First-Time Setup

### 1. Create the Wi-Fi configuration file

The file `include/network_config.h` is **gitignored** so your Wi-Fi credentials
stay out of version control. You must create it manually:

```bash
cat > include/network_config.h << 'EOF'
#pragma once

// --- Station mode (connect to your home/office Wi-Fi) ---
#define WIFI_STA_SSID     "YourWiFiSSID"
#define WIFI_STA_PASSWORD "YourWiFiPassword"

// --- Fallback access point (if station fails or SSID is empty) ---
#define WIFI_AP_SSID     "ESP32-Print-Bridge"
#define WIFI_AP_PASSWORD "printbridge"

// --- mDNS hostname (reachable as <hostname>.local) ---
#define PRINTER_HOSTNAME "esp32-print-bridge"
EOF
```

Replace `YourWiFiSSID` and `YourWiFiPassword` with your actual credentials.
If you leave `WIFI_STA_SSID` empty (`""`), the firmware will skip station mode
and start the fallback access point instead.

### 2. Build and flash

1. Open this folder in Visual Studio Code with the PlatformIO extension.
2. Build with PlatformIO (`pio run`).
3. Upload to the ESP32-S3 (`pio run -t upload`).
4. Open the serial monitor at `115200` baud.
5. Note the printed IP address, or connect to the fallback AP.

### 3. Wire the USB printer

The ESP32-S3 must be wired in **USB host mode** to the printer — this is
separate from the UART/USB bridge used for serial monitor and flashing.

- USB D+ and D- from the ESP32-S3 host-capable pins go to the printer
- The printer side must see valid VBUS power
- The printer should be self-powered or externally powered

### 4. Add the printer on your computer

The ESP32 speaks standard RAW/AppSocket on port `9100`. Your OS's printer driver
does all the rendering — the ESP32 is a transparent byte pipe. Follow the
instructions for your platform below.

#### Windows

1. Open **Settings > Bluetooth & devices > Printers & scanners**.
2. Click **Add device**, then click **Add manually**.
3. Select **Add a printer using a TCP/IP address or hostname**.
4. Enter:
   - **Hostname or IP address:** the ESP32's IP (from serial monitor, e.g. `192.168.1.42`)
   - **Port name:** leave default or give it a name
5. When prompted for device type, select **Custom** and click **Settings...**
   - **Protocol:** Raw
   - **Port Number:** `9100`
6. Click **OK**, then **Next**.
7. Install the driver for your USB printer model (e.g. Canon G1010). Use
   **Have Disk** if you downloaded the driver separately.
8. Print a test page to verify.

#### macOS

1. Open **System Settings > Printers & Scanners** (or **System Preferences > Printers & Scanners** on older macOS).
2. Click the **+** button to add a printer.
3. Select the **IP** tab.
4. Enter:
   - **Address:** the ESP32's IP (e.g. `192.168.1.42`)
   - **Protocol:** HP Jetdirect - Socket
   - **Queue:** leave empty
   - **Name:** anything you like (e.g. `USB Print Bridge`)
5. For **Use / Driver**, choose **Select Software...** and pick your printer
   model, or click **Other...** to load a PPD file from the manufacturer.
6. Click **Add**, then print a test page to verify.

> **Tip:** If the ESP32 is advertising mDNS, you can also use
> `esp32-print-bridge.local` as the address instead of the IP.

#### Linux (CUPS)

1. Open a browser and go to the CUPS web interface at `http://localhost:631`.
2. Click **Administration > Add Printer**.
3. Under **Other Network Printers**, select **AppSocket/HP JetDirect**.
4. Enter the connection URI:
   ```
   socket://192.168.1.42:9100
   ```
   (replace with the ESP32's actual IP)
5. Give the printer a name and description.
6. Select the driver/PPD for your printer model. If your printer is not listed,
   download the PPD from the manufacturer and upload it.
7. Click **Add Printer**, set default options, then print a test page.

Alternatively, add it from the command line:

```bash
# Using CUPS lpadmin (replace IP, name, and PPD path)
lpadmin -p "USB-Print-Bridge" \
  -v "socket://192.168.1.42:9100" \
  -P /path/to/your-printer.ppd \
  -E

# Print a test page
lp -d "USB-Print-Bridge" /usr/share/cups/data/testprint
```

## Network Modes

The firmware tries Wi-Fi **station mode** first using the credentials in
`include/network_config.h`.

If `WIFI_STA_SSID` is left empty, it starts its own **access point**:

| Setting  | Value                        |
|----------|------------------------------|
| SSID     | `ESP32-Print-Bridge`         |
| Password | `printbridge`                |
| Hostname | `esp32-print-bridge.local`   |

## LED Status Guide

| Color / Pattern          | Meaning                     |
|--------------------------|-----------------------------|
| White (solid)            | Booting                     |
| Blue (blinking)          | Connecting to Wi-Fi         |
| Purple (blinking)        | Access point ready           |
| Orange (blinking)        | Waiting for USB printer      |
| Green (solid)            | Ready for print jobs         |
| Cyan (solid)             | Client connected             |
| Yellow (fast blink)      | Receiving / printing data    |
| Green (fast blink)       | Job completed                |
| Red (fast blink)         | Error                        |

## Debug Console

A TCP debug console is available on port `2323`. Connect with:

```bash
nc <esp-ip-address> 2323
```

Commands: `status`, `usb`, `job`, `buf`, `heap`, `help`, `close`.

## Validate RAW Port 9100

Send a test file to the RAW socket:

```bash
nc <esp-ip-address> 9100 < test-print.bin
```

Or a text smoke test:

```bash
printf "test page\n" | nc <esp-ip-address> 9100
```

The serial monitor will log connection and byte-count information while the LED
turns yellow during the transfer.

> **Note:** Smoke tests validate the network path. For a full print validation,
> send a real job through your printer driver (e.g. print a test page from
> Windows with the Canon G1010 driver pointed at the ESP32's IP).

## LED Pin

The onboard RGB LED defaults to `GPIO48` (ESP32-S3-DevKitC-1). If your LED does
not work, change the build flag in `platformio.ini`:

```ini
build_flags = -DRGB_LED_PIN=38
```

## Project Structure

```
include/
  network_config.h      # Wi-Fi credentials (gitignored, create manually)
  ring_buffer.h         # Lock-free ring buffer core
  usb_printer_bridge.h  # USB host printer bridge API
src/
  main.cpp              # Wi-Fi, TCP server, LED, print job orchestration
  usb_printer_bridge.cpp# USB host enumeration, endpoint claim, data transfer
partitions/
  default_16MB.csv      # Custom partition table for 16MB flash
platformio.ini          # PlatformIO build configuration
```
