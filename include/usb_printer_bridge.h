#pragma once

#include <Arduino.h>

struct UsbPrinterBridgeStatus {
  bool host_running = false;
  bool device_connected = false;
  bool printer_ready = false;
  bool backend_faulted = false;
  bool dry_run_mode = false;
  uint8_t device_address = 0;
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  uint8_t interface_number = 0xff;
  uint8_t out_endpoint = 0;
  uint8_t in_endpoint = 0;
  size_t total_forwarded_bytes = 0;
  size_t total_dropped_bytes = 0;
  size_t failed_transfer_count = 0;
  char last_error[128] = {0};
};

namespace usb_printer_bridge {

bool begin();
bool is_ready();
bool has_device();
bool is_faulted();
bool send_raw(const uint8_t *data, size_t length);
int recv_raw(uint8_t *data, size_t max_length, uint32_t timeout_ms);
void get_status(UsbPrinterBridgeStatus *status);
const char *last_error();

}  // namespace usb_printer_bridge
