# ---------------------------------------------------------------------------
# Multi-stage Dockerfile for the ESP32-S3 USB print bridge.
#
# Targets:
#   test   -- runs native unit tests (ring buffer)
#   build  -- compiles the firmware for esp32-s3-n16r8 (default)
#
# Usage:
#   docker build --target build -t esp32-printer-build .
#   docker build --target test  -t esp32-printer-test  .
#   docker compose run build     # writes .pio/build/esp32-s3-n16r8/firmware.bin
#   docker compose run tests     # runs pio test -e native-tests
# ---------------------------------------------------------------------------

# Shared base: Python + PlatformIO Core
FROM python:3.11-slim AS base

RUN apt-get update && apt-get install -y --no-install-recommends \
      git \
      curl \
      ca-certificates \
      udev \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir platformio

WORKDIR /project

# ---------------------------------------------------------------------------
# test stage: only needs the native toolchain
# ---------------------------------------------------------------------------
FROM base AS test

COPY platformio.ini ./
COPY include/ring_buffer.h include/ring_buffer.h
COPY include/usb_printer_bridge.h include/usb_printer_bridge.h
COPY test/ test/

RUN pio pkg install -e native-tests

CMD ["pio", "test", "-e", "native-tests", "-v"]

# ---------------------------------------------------------------------------
# build stage: full ESP32-S3 toolchain + project sources
# ---------------------------------------------------------------------------
FROM base AS build

# Copy everything needed for a firmware build. network_config.h is mounted at
# runtime from the host via docker-compose so credentials never enter the image.
COPY platformio.ini ./
COPY boards/ boards/
COPY partitions/ partitions/
COPY include/ include/
COPY src/ src/
COPY test/ test/

# Pre-install the ESP32-S3 toolchain and libraries so `docker compose run build`
# doesn't redownload them every invocation. This is the slowest step (~5 min on
# a cold build) but only runs once when the image is built.
RUN pio pkg install -e esp32-s3-n16r8 || true

CMD ["pio", "run", "-e", "esp32-s3-n16r8"]
