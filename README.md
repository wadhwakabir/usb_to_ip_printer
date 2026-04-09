# ESP32-S3 N16R8 PlatformIO Starter

This workspace is set up for a generic ESP32-S3 N16R8 board:

- 16 MB flash
- 8 MB octal PSRAM
- Arduino framework
- onboard RGB LED blink example

## Open In VS Code

1. Open this folder in Visual Studio Code.
2. Let PlatformIO finish its first-run setup if prompted.
3. Build with the PlatformIO sidebar or run `PlatformIO: Build`.
4. Plug in the board and use `PlatformIO: Upload`.
5. Open the serial monitor at `115200`.

## LED Pin

The starter code assumes the onboard RGB LED data pin is `GPIO48`, which matches current ESP32-S3-DevKitC-1 documentation.

If your LED does not blink, change the build flag in `/Users/kabuwadhwa/projects/wireless-printer/platformio.ini` from:

```ini
-DRGB_LED_PIN=48
```

to:

```ini
-DRGB_LED_PIN=38
```

Some older ESP32-S3 board revisions use `GPIO38`.
