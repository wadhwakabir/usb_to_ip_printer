# ESP32-S3 RAW TCP Print Server

This workspace is set up for a generic ESP32-S3 N16R8 board:

- 16 MB flash
- 8 MB octal PSRAM
- Arduino framework
- RAW TCP print server on port `9100`
- RGB LED status feedback for printer state

## Network Modes

The firmware tries Wi-Fi station mode first if you set credentials in
[`include/network_config.h`](/Users/kabuwadhwa/projects/wireless-printer/include/network_config.h).

If `WIFI_STA_SSID` is left empty, it starts its own access point:

- SSID: `ESP32-Print-Bridge`
- Password: `printbridge`
- Hostname: `esp32-print-bridge.local`

## LED States

- white: booting
- blue blinking: connecting to Wi-Fi
- purple blinking: access point ready
- green: ready for connections
- cyan: client connected
- yellow blinking: receiving RAW print data
- green fast blink: job completed
- red blinking: error

## Build And Flash

1. Open this folder in Visual Studio Code.
2. Build with PlatformIO.
3. Upload to the ESP32-S3.
4. Open the serial monitor at `115200`.
5. Note the printed IP address or connect to the fallback access point.

## Validate RAW Port 9100

Send a file to the RAW socket from your computer:

```bash
nc <esp-ip-address> 9100 < test-print.bin
```

Or send a text smoke test:

```bash
printf "test page\n" | nc <esp-ip-address> 9100
```

The serial monitor will log connection and byte-count information while the LED
turns yellow during the transfer.

## Current Printer Backend

The Wi-Fi RAW print ingress is implemented now, and the firmware also includes a
USB host printer backend for forwarding RAW `9100` data to a connected USB
printer.

If no USB printer is attached, incoming RAW jobs now fail instead of being
silently accepted and dropped.

Once a USB printer is attached and enumerated successfully, the bridge forwards
incoming RAW data over the claimed bulk OUT endpoint.

## USB Host Wiring Notes

The ESP32-S3 must be wired in USB host mode to the printer, not just connected
to your computer for flashing. In practice that means:

- USB D+ and D- from the ESP32-S3 host-capable pins must go to the printer
- the printer side must see valid VBUS power
- the printer should preferably be self-powered or externally powered

Your current serial monitor/upload path uses the UART/USB bridge on the dev
board, which is separate from the printer-facing USB host path.

## Canon G1010 Notes

The Canon PIXMA G1010 is a USB printer that depends on Canon's host-side driver.
The intended end-to-end path is:

1. Windows machine with the Canon G1010 driver installed
2. Printer pointed at the ESP32-S3 as a RAW TCP printer on port `9100`
3. ESP32-S3 forwards those bytes to the USB-connected printer

Plain `nc` smoke tests are still useful for validating the network socket, but
they are not a full print validation for the Canon driver path.

## LED Pin

The onboard RGB LED defaults to `GPIO48`, which matches current ESP32-S3-DevKitC-1 documentation.

If your LED does not behave correctly, change the build flag in
[`platformio.ini`](/Users/kabuwadhwa/projects/wireless-printer/platformio.ini)
from `-DRGB_LED_PIN=48` to `-DRGB_LED_PIN=38`.
