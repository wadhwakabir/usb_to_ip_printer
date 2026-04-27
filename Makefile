# ---------------------------------------------------------------------------
# ESP32-S3 USB-to-IP print bridge — developer Makefile
#
# For brand-new users the path is:
#
#     git clone <repo> && cd usb_to_ip_printer
#     make build          # compile firmware
#     make flash          # flash to ESP32-S3 over USB
#     make monitor        # open serial monitor
#
# Wi-Fi credentials are not compiled in.  After flashing, the device brings
# up its own AP (see setup.txt) and the end user enters their SSID/password
# via the captive portal.  Every unit ships identical.
#
# All targets work both with a local PlatformIO install and inside Docker.
# The Makefile auto-detects which is available and prefers the local install.
# Force Docker with:  make build USE_DOCKER=1
# ---------------------------------------------------------------------------

ENV          ?= esp32-s3-n16r8
TEST_ENV     ?= native-tests

# Prefer a locally installed pio; fall back to the common PlatformIO Penv path.
PIO_LOCAL := $(shell command -v pio 2>/dev/null)
ifeq ($(PIO_LOCAL),)
  PIO_LOCAL := $(shell test -x $$HOME/.platformio/penv/bin/pio && echo $$HOME/.platformio/penv/bin/pio)
endif

DOCKER := $(shell command -v docker 2>/dev/null)

# Decide whether to run commands locally or via Docker.
ifeq ($(USE_DOCKER),1)
  RUNNER_BUILD := docker compose run --rm build
  RUNNER_TEST  := docker compose run --rm tests
else ifneq ($(PIO_LOCAL),)
  RUNNER_BUILD := $(PIO_LOCAL)
  RUNNER_TEST  := $(PIO_LOCAL)
else ifneq ($(DOCKER),)
  RUNNER_BUILD := docker compose run --rm build
  RUNNER_TEST  := docker compose run --rm tests
else
  $(error Neither a local `pio` nor `docker` was found. Install PlatformIO (`pip install platformio`) or Docker and retry)
endif

.PHONY: help build flash monitor upload dev test docker-build docker-test docker-image clean distclean ports doctor require-local-pio

# ---------------------------------------------------------------------------
# Help (default target)
# ---------------------------------------------------------------------------
help:
	@echo "ESP32-S3 USB-to-IP print bridge"
	@echo ""
	@echo "Quick start:"
	@echo "  make build         Compile firmware"
	@echo "  make flash         Compile + flash to connected ESP32-S3"
	@echo "  make monitor       Open serial monitor (115200 baud)"
	@echo "  make test          Run native unit tests"
	@echo ""
	@echo "Docker-only workflow (no local PlatformIO needed):"
	@echo "  make docker-image  Build both Docker images (test + build)"
	@echo "  make docker-build  Build firmware in container"
	@echo "  make docker-test   Run tests in container"
	@echo ""
	@echo "Utilities:"
	@echo "  make ports         List USB serial ports on this machine"
	@echo "  make doctor        Check local environment for required tools"
	@echo "  make clean         Remove build artefacts"
	@echo "  make distclean     Remove build artefacts + Docker images"
	@echo ""
	@echo "Current runner: $(RUNNER_BUILD)"

# ---------------------------------------------------------------------------
# Build firmware
# ---------------------------------------------------------------------------
build:
	$(RUNNER_BUILD) run -e $(ENV)

# ---------------------------------------------------------------------------
# Flash + optional serial monitor
# ---------------------------------------------------------------------------
# Flashing requires USB passthrough, which Docker doesn't handle cleanly on
# macOS — so flash/monitor always use the local pio install.
flash: require-local-pio
	$(PIO_LOCAL) run -e $(ENV) --target upload

upload: flash

monitor: require-local-pio
	$(PIO_LOCAL) device monitor

# Combined build + flash + monitor, like a typical edit/flash/debug loop
dev: require-local-pio
	$(PIO_LOCAL) run -e $(ENV) --target upload && $(PIO_LOCAL) device monitor

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
test:
	$(RUNNER_TEST) test -e $(TEST_ENV)

# ---------------------------------------------------------------------------
# Docker-specific targets (force Docker regardless of local install)
# ---------------------------------------------------------------------------
docker-image:
	docker compose build

docker-build:
	docker compose run --rm build

docker-test:
	docker compose run --rm tests

# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------
ports:
	@echo "USB serial ports (use the matching device in platformio.ini):"
	@if [ "$$(uname)" = "Darwin" ]; then \
	  ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB_* 2>/dev/null || echo "  (none found)"; \
	else \
	  ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "  (none found)"; \
	fi

doctor:
	@echo "Environment check:"
	@printf "  PlatformIO:     "; $(PIO_LOCAL) --version 2>/dev/null || echo "not installed (install with: pip install platformio)"
	@printf "  Docker:         "; $(DOCKER) --version 2>/dev/null || echo "not installed"
	@printf "  Runner:         "; echo "$(RUNNER_BUILD)"
	@$(MAKE) --no-print-directory ports

require-local-pio:
	@if [ -z "$(PIO_LOCAL)" ]; then \
	  echo "ERROR: flash/monitor need a local PlatformIO install."; \
	  echo "       Install with:  pip install platformio"; \
	  echo "       (Docker cannot reliably forward USB for flashing.)"; \
	  exit 1; \
	fi

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
clean:
	rm -rf .pio/build

distclean: clean
	-docker image rm usb_to_ip_printer-build usb_to_ip_printer-tests 2>/dev/null || true
	rm -rf .pio
