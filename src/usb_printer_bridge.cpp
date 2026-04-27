#include "usb_printer_bridge.h"

#include <stdarg.h>
#include <string.h>

#include <Arduino.h>
extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
}

namespace usb_printer_bridge {
namespace {

constexpr uint8_t kPrinterClassCode = USB_CLASS_PRINTER;
constexpr bool kAllowDryRunWithoutPrinter = false;
constexpr size_t kTransferBufferSize = 4096;
constexpr size_t kInTransferBufferSize = 1024;
constexpr TickType_t kTransferTimeoutTicks = pdMS_TO_TICKS(15000);

struct BridgeState {
  SemaphoreHandle_t state_mutex = nullptr;
  SemaphoreHandle_t send_mutex = nullptr;
  SemaphoreHandle_t transfer_done = nullptr;

  TaskHandle_t lib_task = nullptr;
  TaskHandle_t client_task = nullptr;

  usb_host_client_handle_t client_hdl = nullptr;
  usb_device_handle_t device_hdl = nullptr;
  usb_transfer_t *out_transfer = nullptr;
  usb_transfer_t *in_transfer = nullptr;
  SemaphoreHandle_t in_transfer_done = nullptr;
  SemaphoreHandle_t in_mutex = nullptr;
  volatile bool in_transfer_in_flight = false;
  usb_transfer_status_t last_in_transfer_status = USB_TRANSFER_STATUS_COMPLETED;
  int last_in_actual_num_bytes = 0;

  bool host_running = false;
  bool client_registered = false;
  bool device_connected = false;
  bool printer_ready = false;
  bool backend_faulted = false;
  bool dry_run_mode = kAllowDryRunWithoutPrinter;

  // Small FIFO of device addresses announced by USB_HOST_CLIENT_EVENT_NEW_DEV
  // that the client task has not yet opened.  A single-slot design would lose
  // events when a hub enumerates multiple devices back-to-back: the callback
  // runs in USB-lib context and cannot block, so we must queue here.
  static constexpr size_t kPendingDeviceCapacity = 4;
  uint8_t pending_device_addrs[kPendingDeviceCapacity] = {0};
  size_t pending_device_count = 0;
  bool device_gone_pending = false;

  uint8_t device_address = 0;
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  uint8_t interface_number = 0xff;
  uint8_t out_endpoint = 0;
  uint8_t in_endpoint = 0;
  uint16_t out_endpoint_mps = 0;

  size_t total_forwarded_bytes = 0;
  size_t total_dropped_bytes = 0;
  size_t failed_transfer_count = 0;

  usb_transfer_status_t last_transfer_status = USB_TRANSFER_STATUS_COMPLETED;
  int last_transfer_actual_num_bytes = 0;
  volatile bool transfer_in_flight = false;

  char last_error[128] = "USB host not started";
};

BridgeState g_state;

String usb_string_to_ascii(const usb_str_desc_t *desc) {
  if (desc == nullptr || desc->bLength < 2) {
    return String("(none)");
  }

  String result;
  const int length = (desc->bLength - 2) / 2;
  for (int i = 0; i < length; ++i) {
    const char c = static_cast<char>(desc->wData[i] & 0xFF);
    result += (c >= 32 && c <= 126) ? c : '?';
  }
  return result;
}

void set_last_error_locked(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(g_state.last_error, sizeof(g_state.last_error), format, args);
  va_end(args);
}

void set_last_error(const char *format, ...) {
  if (g_state.state_mutex == nullptr) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_state.last_error, sizeof(g_state.last_error), format, args);
    va_end(args);
    return;
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  va_list args;
  va_start(args, format);
  vsnprintf(g_state.last_error, sizeof(g_state.last_error), format, args);
  va_end(args);
  xSemaphoreGive(g_state.state_mutex);
}

void transfer_callback(usb_transfer_t *transfer) {
  auto *state = static_cast<BridgeState *>(transfer->context);
  if (state == nullptr) {
    return;
  }

  state->last_transfer_status = transfer->status;
  state->last_transfer_actual_num_bytes = transfer->actual_num_bytes;
  state->transfer_in_flight = false;
  xSemaphoreGive(state->transfer_done);
}

void in_transfer_callback(usb_transfer_t *transfer) {
  auto *state = static_cast<BridgeState *>(transfer->context);
  if (state == nullptr) {
    return;
  }

  state->last_in_transfer_status = transfer->status;
  state->last_in_actual_num_bytes = transfer->actual_num_bytes;
  state->in_transfer_in_flight = false;
  xSemaphoreGive(state->in_transfer_done);
}

void set_printer_ready_message_locked() {
  if (g_state.printer_ready && g_state.interface_number != 0xff &&
      g_state.out_endpoint != 0) {
    set_last_error_locked("USB printer ready on interface %u endpoint 0x%02x",
                          g_state.interface_number, g_state.out_endpoint);
  }
}

void recover_endpoint_locked(const char *reason) {
  Serial.printf("[USB] Recovering endpoint after %s\n", reason);
  if (g_state.device_hdl != nullptr && g_state.out_endpoint != 0) {
    usb_host_endpoint_halt(g_state.device_hdl, g_state.out_endpoint);
    usb_host_endpoint_flush(g_state.device_hdl, g_state.out_endpoint);
    usb_host_endpoint_clear(g_state.device_hdl, g_state.out_endpoint);
  }
  g_state.transfer_in_flight = false;
  while (xSemaphoreTake(g_state.transfer_done, 0) == pdTRUE) {
  }
}

bool inspect_interfaces(usb_device_handle_t device_hdl) {
  const usb_config_desc_t *config_desc = nullptr;
  esp_err_t err = usb_host_get_active_config_descriptor(device_hdl, &config_desc);
  if (err != ESP_OK || config_desc == nullptr) {
    Serial.printf("[USB] Failed to get config descriptor: %s\n",
                  esp_err_to_name(err));
    set_last_error("USB config descriptor unavailable: %s", esp_err_to_name(err));
    return false;
  }

  bool found_any_bulk_out = false;
  bool found_printer_interface = false;

  uint8_t selected_interface = 0xff;
  uint8_t selected_alt = 0;
  uint8_t selected_out = 0;
  uint8_t selected_in = 0;
  uint16_t selected_mps = 0;

  const usb_standard_desc_t *cursor =
      reinterpret_cast<const usb_standard_desc_t *>(config_desc);
  int offset = 0;

  while ((cursor = usb_parse_next_descriptor(cursor, config_desc->wTotalLength,
                                             &offset)) != nullptr) {
    if (cursor->bDescriptorType != USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      continue;
    }

    const auto *intf_desc = reinterpret_cast<const usb_intf_desc_t *>(cursor);
    Serial.printf("[USB] Interface %u alt %u class=0x%02x subclass=0x%02x "
                  "proto=0x%02x endpoints=%u\n",
                  intf_desc->bInterfaceNumber, intf_desc->bAlternateSetting,
                  intf_desc->bInterfaceClass, intf_desc->bInterfaceSubClass,
                  intf_desc->bInterfaceProtocol, intf_desc->bNumEndpoints);

    uint8_t bulk_out = 0;
    uint8_t bulk_in = 0;
    uint16_t bulk_out_mps = 0;

    for (int i = 0; i < intf_desc->bNumEndpoints; ++i) {
      int ep_offset = offset;
      const usb_ep_desc_t *ep_desc = usb_parse_endpoint_descriptor_by_index(
          intf_desc, i, config_desc->wTotalLength, &ep_offset);
      if (ep_desc == nullptr) {
        continue;
      }

      Serial.printf("[USB]  Endpoint 0x%02x type=%u mps=%u interval=%u\n",
                    ep_desc->bEndpointAddress, USB_EP_DESC_GET_XFERTYPE(ep_desc),
                    USB_EP_DESC_GET_MPS(ep_desc), ep_desc->bInterval);

      if (USB_EP_DESC_GET_XFERTYPE(ep_desc) != USB_TRANSFER_TYPE_BULK) {
        continue;
      }

      if (USB_EP_DESC_GET_EP_DIR(ep_desc) == 0 && bulk_out == 0) {
        bulk_out = ep_desc->bEndpointAddress;
        bulk_out_mps = USB_EP_DESC_GET_MPS(ep_desc);
        found_any_bulk_out = true;
      } else if (USB_EP_DESC_GET_EP_DIR(ep_desc) != 0 && bulk_in == 0) {
        bulk_in = ep_desc->bEndpointAddress;
      }
    }

    const bool is_printer_class =
        intf_desc->bInterfaceClass == kPrinterClassCode;
    if (bulk_out == 0) {
      continue;
    }

    if (is_printer_class) {
      selected_interface = intf_desc->bInterfaceNumber;
      selected_alt = intf_desc->bAlternateSetting;
      selected_out = bulk_out;
      selected_in = bulk_in;
      selected_mps = bulk_out_mps;
      found_printer_interface = true;
      break;
    }

    if (selected_interface == 0xff) {
      selected_interface = intf_desc->bInterfaceNumber;
      selected_alt = intf_desc->bAlternateSetting;
      selected_out = bulk_out;
      selected_in = bulk_in;
      selected_mps = bulk_out_mps;
    }
  }

  if (selected_interface == 0xff || selected_out == 0) {
    if (found_any_bulk_out) {
      set_last_error("USB device has bulk OUT but no usable interface selected");
    } else {
      set_last_error("USB device has no bulk OUT endpoint");
    }
    return false;
  }

  if (!found_printer_interface) {
    Serial.println("[USB] No printer-class interface found; falling back to "
                   "first bulk OUT interface");
  }

  err = usb_host_interface_claim(g_state.client_hdl, device_hdl,
                                 selected_interface, selected_alt);
  if (err != ESP_OK) {
    Serial.printf("[USB] Failed to claim interface %u alt %u: %s\n",
                  selected_interface, selected_alt, esp_err_to_name(err));
    set_last_error("USB interface claim failed: %s", esp_err_to_name(err));
    return false;
  }

  if (g_state.out_transfer == nullptr) {
    err = usb_host_transfer_alloc(kTransferBufferSize, 0, &g_state.out_transfer);
    if (err != ESP_OK || g_state.out_transfer == nullptr) {
      Serial.printf("[USB] Failed to allocate OUT transfer buffer: %s\n",
                    esp_err_to_name(err));
      usb_host_interface_release(g_state.client_hdl, device_hdl,
                                 selected_interface);
      set_last_error("USB OUT transfer alloc failed: %s", esp_err_to_name(err));
      return false;
    }
  }

  if (g_state.in_transfer == nullptr && selected_in != 0) {
    err = usb_host_transfer_alloc(kInTransferBufferSize, 0, &g_state.in_transfer);
    if (err != ESP_OK || g_state.in_transfer == nullptr) {
      Serial.printf("[USB] Failed to allocate IN transfer buffer: %s (non-fatal)\n",
                    esp_err_to_name(err));
      g_state.in_transfer = nullptr;
    }
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  g_state.interface_number = selected_interface;
  g_state.out_endpoint = selected_out;
  g_state.in_endpoint = selected_in;
  g_state.out_endpoint_mps = selected_mps;
  g_state.printer_ready = true;
  g_state.backend_faulted = false;
  set_printer_ready_message_locked();
  xSemaphoreGive(g_state.state_mutex);

  Serial.printf("[USB] Printer backend ready on interface %u alt %u OUT 0x%02x"
                " IN 0x%02x MPS %u\n",
                selected_interface, selected_alt, selected_out, selected_in,
                selected_mps);
  return true;
}

void close_active_device() {
  if (g_state.device_hdl == nullptr) {
    xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
    g_state.device_connected = false;
    g_state.printer_ready = false;
    g_state.backend_faulted = false;
    g_state.device_address = 0;
    g_state.vendor_id = 0;
    g_state.product_id = 0;
    g_state.interface_number = 0xff;
    g_state.out_endpoint = 0;
    g_state.in_endpoint = 0;
    g_state.out_endpoint_mps = 0;
    xSemaphoreGive(g_state.state_mutex);
    return;
  }

  // Mark printer as not ready so recv_raw() will bail out early and not
  // submit new IN transfers while we're tearing down.
  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  g_state.printer_ready = false;
  xSemaphoreGive(g_state.state_mutex);

  // Serialize with recv_raw() via in_mutex.  Clearing printer_ready alone is
  // not enough: a recv_raw() that already passed its printer_ready check and
  // captured dev_hdl can race forward to usb_host_transfer_submit() on a
  // handle we're about to close.  Taking in_mutex blocks new recv_raw()
  // callers, and since recv_raw() holds in_mutex across its submit+wait, we
  // are guaranteed no transfer is in flight once the take succeeds.
  xSemaphoreTake(g_state.in_mutex, portMAX_DELAY);

  // Even under in_mutex the callback could set in_transfer_in_flight=false
  // slightly before the USB stack has fully released the transfer buffer.
  // Keep the bounded wait as a belt-and-braces check.
  if (g_state.in_transfer_in_flight) {
    Serial.println("[USB] Waiting for in-flight IN transfer to finish");
    for (int i = 0; i < 20 && g_state.in_transfer_in_flight; ++i) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (g_state.in_transfer_in_flight) {
      Serial.println("[USB] IN transfer still in flight after wait; proceeding with close");
      g_state.in_transfer_in_flight = false;
    }
  }

  if (g_state.interface_number != 0xff) {
    usb_host_interface_release(g_state.client_hdl, g_state.device_hdl,
                               g_state.interface_number);
  }

  usb_host_device_close(g_state.client_hdl, g_state.device_hdl);

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  g_state.device_hdl = nullptr;
  g_state.device_connected = false;
  g_state.printer_ready = false;
  g_state.backend_faulted = false;
  g_state.device_address = 0;
  g_state.vendor_id = 0;
  g_state.product_id = 0;
  g_state.interface_number = 0xff;
  g_state.out_endpoint = 0;
  g_state.in_endpoint = 0;
  g_state.out_endpoint_mps = 0;
  g_state.transfer_in_flight = false;
  set_last_error_locked("USB printer disconnected");
  xSemaphoreGive(g_state.state_mutex);

  xSemaphoreGive(g_state.in_mutex);
}

void handle_new_device(uint8_t device_address) {
  if (g_state.device_hdl != nullptr) {
    Serial.printf("[USB] Ignoring device %u because one USB device is already "
                  "managed\n",
                  device_address);
    return;
  }

  usb_device_handle_t device_hdl = nullptr;
  esp_err_t err =
      usb_host_device_open(g_state.client_hdl, device_address, &device_hdl);
  if (err != ESP_OK) {
    Serial.printf("[USB] Failed to open device %u: %s\n", device_address,
                  esp_err_to_name(err));
    set_last_error("USB device open failed: %s", esp_err_to_name(err));
    return;
  }

  usb_device_info_t device_info{};
  err = usb_host_device_info(device_hdl, &device_info);
  if (err != ESP_OK) {
    Serial.printf("[USB] Failed to fetch device info: %s\n",
                  esp_err_to_name(err));
    usb_host_device_close(g_state.client_hdl, device_hdl);
    set_last_error("USB device info failed: %s", esp_err_to_name(err));
    return;
  }

  const usb_device_desc_t *device_desc = nullptr;
  err = usb_host_get_device_descriptor(device_hdl, &device_desc);
  if (err != ESP_OK || device_desc == nullptr) {
    Serial.printf("[USB] Failed to fetch device descriptor: %s\n",
                  esp_err_to_name(err));
    usb_host_device_close(g_state.client_hdl, device_hdl);
    set_last_error("USB device descriptor failed: %s", esp_err_to_name(err));
    return;
  }

  const String manufacturer =
      usb_string_to_ascii(device_info.str_desc_manufacturer);
  const String product = usb_string_to_ascii(device_info.str_desc_product);

  Serial.printf("[USB] Device connected addr=%u VID=%04x PID=%04x product=\"%s\" "
                "manufacturer=\"%s\"\n",
                device_address, device_desc->idVendor, device_desc->idProduct,
                product.c_str(), manufacturer.c_str());

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  g_state.device_hdl = device_hdl;
  g_state.device_connected = true;
  g_state.device_address = device_address;
  g_state.vendor_id = device_desc->idVendor;
  g_state.product_id = device_desc->idProduct;
  g_state.printer_ready = false;
  g_state.backend_faulted = false;
  set_last_error_locked("USB device attached, probing interfaces");
  xSemaphoreGive(g_state.state_mutex);

  if (!inspect_interfaces(device_hdl)) {
    Serial.println("[USB] Device attached but no supported printer path found "
                   "yet");
  }
}

void client_event_callback(const usb_host_client_event_msg_t *event_msg,
                           void *arg) {
  (void)arg;
  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
      xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
      const uint8_t addr = event_msg->new_dev.address;
      // Skip if this address is already queued (defensive: the USB host lib
      // should not double-announce, but treat the queue as a set).
      bool already_queued = false;
      for (size_t i = 0; i < g_state.pending_device_count; ++i) {
        if (g_state.pending_device_addrs[i] == addr) {
          already_queued = true;
          break;
        }
      }
      if (!already_queued) {
        if (g_state.pending_device_count <
            BridgeState::kPendingDeviceCapacity) {
          g_state.pending_device_addrs[g_state.pending_device_count++] = addr;
        } else {
          // Queue full — drop and log later via client task.  Set a last_error
          // so the user can see it in the debug console.
          set_last_error_locked(
              "Dropped NEW_DEV event (addr=%u): pending queue full", addr);
        }
      }
      xSemaphoreGive(g_state.state_mutex);
      break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
      g_state.device_gone_pending = true;
      xSemaphoreGive(g_state.state_mutex);
      break;
  }
}

void usb_lib_daemon_task(void *arg) {
  (void)arg;
  while (true) {
    uint32_t event_flags = 0;
    const esp_err_t err =
        usb_host_lib_handle_events(pdMS_TO_TICKS(1000), &event_flags);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      Serial.printf("[USB] Host daemon error: %s\n", esp_err_to_name(err));
    }
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      Serial.println("[USB] Host reports no registered clients");
    }
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
      Serial.println("[USB] Host reports all devices freed");
    }
  }
}

void usb_client_task(void *arg) {
  (void)arg;

  const usb_host_client_config_t client_config = {
      .is_synchronous = false,
      .max_num_event_msg = 5,
      .async =
          {
              .client_event_callback = client_event_callback,
              .callback_arg = nullptr,
          },
  };

  usb_host_client_handle_t client_hdl = nullptr;
  esp_err_t err = usb_host_client_register(&client_config, &client_hdl);
  if (err != ESP_OK) {
    Serial.printf("[USB] Client register failed: %s\n", esp_err_to_name(err));
    set_last_error("USB client register failed: %s", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  g_state.client_hdl = client_hdl;
  g_state.client_registered = true;
  xSemaphoreGive(g_state.state_mutex);

  Serial.println("[USB] Host client registered");

  while (true) {
    err = usb_host_client_handle_events(client_hdl, pdMS_TO_TICKS(100));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      Serial.printf("[USB] Client handle_events error: %s\n",
                    esp_err_to_name(err));
    }

    bool should_close = false;
    uint8_t device_address = 0;
    bool have_pending_open = false;

    xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
    if (g_state.pending_device_count > 0) {
      device_address = g_state.pending_device_addrs[0];
      have_pending_open = true;
    }
    if (g_state.device_gone_pending) {
      should_close = true;
    }
    xSemaphoreGive(g_state.state_mutex);

    // Handle close BEFORE open so a departing device is torn down before a
    // newly-attached one is opened.  If the close cannot proceed right now
    // (send_mutex held by an active transfer) defer the open as well —
    // the pending flags stay set and will be retried next iteration.
    if (should_close) {
      if (xSemaphoreTake(g_state.send_mutex, 0) == pdTRUE) {
        Serial.println("[USB] Device gone event received");
        close_active_device();
        xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
        g_state.device_gone_pending = false;
        xSemaphoreGive(g_state.state_mutex);
        xSemaphoreGive(g_state.send_mutex);
      } else {
        have_pending_open = false;
      }
    }

    if (have_pending_open) {
      handle_new_device(device_address);
      // Pop the address we just handled (FIFO shift).  handle_new_device()
      // may have ignored the device if one was already managed, but either
      // way we've consumed the event.
      xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
      if (g_state.pending_device_count > 0 &&
          g_state.pending_device_addrs[0] == device_address) {
        for (size_t i = 1; i < g_state.pending_device_count; ++i) {
          g_state.pending_device_addrs[i - 1] = g_state.pending_device_addrs[i];
        }
        g_state.pending_device_count--;
      }
      xSemaphoreGive(g_state.state_mutex);
    }
  }
}

// State updates from the OUT path run while holding send_mutex, but
// get_status()/is_faulted()/is_ready() read the same fields under
// state_mutex.  Funnel counter and fault writes through these helpers so the
// two lock domains agree on what a reader sees.
void record_transfer_success_locked(size_t length) {
  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  g_state.total_forwarded_bytes += length;
  g_state.backend_faulted = false;
  set_printer_ready_message_locked();
  xSemaphoreGive(g_state.state_mutex);
}

void record_transfer_failure(const char *fmt, ...) {
  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  g_state.failed_transfer_count += 1;
  g_state.backend_faulted = true;
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_state.last_error, sizeof(g_state.last_error), fmt, args);
  va_end(args);
  xSemaphoreGive(g_state.state_mutex);
}

bool submit_transfer_locked(const uint8_t *data, size_t length) {
  if (g_state.device_hdl == nullptr || !g_state.printer_ready ||
      g_state.out_transfer == nullptr || g_state.out_endpoint == 0) {
    set_last_error("USB printer is not ready");
    return false;
  }

  while (xSemaphoreTake(g_state.transfer_done, 0) == pdTRUE) {
  }

  usb_transfer_t *transfer = g_state.out_transfer;
  if (g_state.transfer_in_flight) {
    record_transfer_failure("USB transfer is still in flight");
    return false;
  }

  memcpy(transfer->data_buffer, data, length);
  transfer->device_handle = g_state.device_hdl;
  transfer->bEndpointAddress = g_state.out_endpoint;
  transfer->num_bytes = static_cast<int>(length);
  transfer->callback = transfer_callback;
  transfer->context = &g_state;
  transfer->timeout_ms = 0;
  // Each OUT write is already submitted as its own USB transfer. Appending a
  // zero-length packet to every exact-MPS chunk can prematurely terminate or
  // confuse printer-side framing, so keep the host transfer flags clear here.
  transfer->flags = 0;

  g_state.transfer_in_flight = true;
  esp_err_t err = usb_host_transfer_submit(transfer);
  if (err != ESP_OK) {
    g_state.transfer_in_flight = false;
    Serial.printf("[USB] Transfer submit failed: %s\n", esp_err_to_name(err));
    record_transfer_failure("USB transfer submit failed: %s",
                            esp_err_to_name(err));
    return false;
  }

  if (xSemaphoreTake(g_state.transfer_done, kTransferTimeoutTicks) != pdTRUE) {
    Serial.println("[USB] Transfer timed out waiting for completion");
    record_transfer_failure("USB transfer completion timed out");
    recover_endpoint_locked("timeout");
    return false;
  }

  if (g_state.last_transfer_status != USB_TRANSFER_STATUS_COMPLETED ||
      g_state.last_transfer_actual_num_bytes != static_cast<int>(length)) {
    Serial.printf("[USB] Transfer failed status=%d actual=%d expected=%u\n",
                  g_state.last_transfer_status,
                  g_state.last_transfer_actual_num_bytes,
                  static_cast<unsigned>(length));
    record_transfer_failure("USB transfer failed status=%d actual=%d",
                            g_state.last_transfer_status,
                            g_state.last_transfer_actual_num_bytes);
    recover_endpoint_locked("transfer error");
    return false;
  }

  record_transfer_success_locked(length);
  return true;
}

}  // namespace

bool begin() {
  if (g_state.host_running) {
    return true;
  }

  g_state.state_mutex = xSemaphoreCreateMutex();
  g_state.send_mutex = xSemaphoreCreateMutex();
  g_state.transfer_done = xSemaphoreCreateBinary();
  g_state.in_mutex = xSemaphoreCreateMutex();
  g_state.in_transfer_done = xSemaphoreCreateBinary();

  if (g_state.state_mutex == nullptr || g_state.send_mutex == nullptr ||
      g_state.transfer_done == nullptr || g_state.in_mutex == nullptr ||
      g_state.in_transfer_done == nullptr) {
    set_last_error("Failed to create USB bridge synchronization primitives");
    return false;
  }

  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };

  const esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    Serial.printf("[USB] Host install failed: %s\n", esp_err_to_name(err));
    set_last_error("USB host install failed: %s", esp_err_to_name(err));
    return false;
  }

  g_state.host_running = true;
  set_last_error("USB host installed");
  Serial.println("[USB] Host library installed");

  xTaskCreate(usb_lib_daemon_task, "usb_lib", 4096, nullptr, 20,
              &g_state.lib_task);
  xTaskCreate(usb_client_task, "usb_client", 6144, nullptr, 19,
              &g_state.client_task);
  if (g_state.lib_task == nullptr || g_state.client_task == nullptr) {
    set_last_error("USB task creation failed");
    return false;
  }
  return true;
}

bool is_ready() {
  bool ready = false;
  if (g_state.state_mutex == nullptr) {
    return false;
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  ready = g_state.printer_ready;
  xSemaphoreGive(g_state.state_mutex);
  return ready;
}

bool has_device() {
  bool connected = false;
  if (g_state.state_mutex == nullptr) {
    return false;
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  connected = g_state.device_connected;
  xSemaphoreGive(g_state.state_mutex);
  return connected;
}

bool is_faulted() {
  bool faulted = false;
  if (g_state.state_mutex == nullptr) {
    return false;
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  faulted = g_state.backend_faulted;
  xSemaphoreGive(g_state.state_mutex);
  return faulted;
}

bool is_ready_and_healthy() {
  if (g_state.state_mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  const bool ok = g_state.printer_ready && !g_state.backend_faulted;
  xSemaphoreGive(g_state.state_mutex);
  return ok;
}

bool send_raw(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0) {
    return false;
  }

  if (!g_state.host_running) {
    set_last_error("USB host is not running");
    return false;
  }

  if (!has_device()) {
    if (kAllowDryRunWithoutPrinter) {
      xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
      g_state.total_dropped_bytes += length;
      set_last_error_locked("No USB printer attached, dry-run drop count=%u",
                            static_cast<unsigned>(g_state.total_dropped_bytes));
      xSemaphoreGive(g_state.state_mutex);
      Serial.printf("[USB] Dry-run: dropped %u bytes because no printer is "
                    "attached\n",
                    static_cast<unsigned>(length));
      return true;
    }
    set_last_error("No USB printer attached");
    return false;
  }

  if (!is_ready()) {
    set_last_error("USB device attached but no printer endpoint is ready");
    return false;
  }

  xSemaphoreTake(g_state.send_mutex, portMAX_DELAY);
  bool ok = true;
  size_t offset = 0;
  size_t maxChunk =
      g_state.out_endpoint_mps > 0 ? g_state.out_endpoint_mps : kTransferBufferSize;
  if (maxChunk > kTransferBufferSize) {
    maxChunk = kTransferBufferSize;
  }
  while (offset < length) {
    const size_t chunk = min(maxChunk, length - offset);
    if (!submit_transfer_locked(data + offset, chunk)) {
      ok = false;
      break;
    }
    offset += chunk;
  }
  xSemaphoreGive(g_state.send_mutex);
  return ok;
}

int recv_raw(uint8_t *data, size_t max_length, uint32_t timeout_ms) {
  if (data == nullptr || max_length == 0) {
    return -1;
  }

  if (!g_state.host_running || g_state.in_transfer == nullptr) {
    return -1;
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  const bool ready = g_state.printer_ready;
  const uint8_t in_ep_probe = g_state.in_endpoint;
  usb_device_handle_t dev_hdl = g_state.device_hdl;
  xSemaphoreGive(g_state.state_mutex);

  if (!ready || in_ep_probe == 0 || dev_hdl == nullptr) {
    return -1;
  }

  xSemaphoreTake(g_state.in_mutex, portMAX_DELAY);

  // Double-check: the device may have disconnected (or been swapped) between
  // releasing state_mutex and acquiring in_mutex.  close_active_device() sets
  // printer_ready=false and waits for in-flight transfers, so re-checking
  // under in_mutex prevents using a stale device handle or endpoint address
  // that no longer belongs to the current device.
  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  if (!g_state.printer_ready || g_state.device_hdl == nullptr ||
      g_state.in_endpoint == 0) {
    xSemaphoreGive(g_state.state_mutex);
    xSemaphoreGive(g_state.in_mutex);
    return -1;
  }
  // Refresh dev_hdl and endpoint address from current state under lock
  dev_hdl = g_state.device_hdl;
  const uint8_t in_ep = g_state.in_endpoint;
  xSemaphoreGive(g_state.state_mutex);

  while (xSemaphoreTake(g_state.in_transfer_done, 0) == pdTRUE) {
  }

  usb_transfer_t *transfer = g_state.in_transfer;
  transfer->device_handle = dev_hdl;
  transfer->bEndpointAddress = in_ep;
  transfer->num_bytes = static_cast<int>(
      max_length < kInTransferBufferSize ? max_length : kInTransferBufferSize);
  transfer->callback = in_transfer_callback;
  transfer->context = &g_state;
  transfer->timeout_ms = timeout_ms;
  transfer->flags = 0;

  g_state.in_transfer_in_flight = true;
  esp_err_t err = usb_host_transfer_submit(transfer);
  if (err != ESP_OK) {
    g_state.in_transfer_in_flight = false;
    xSemaphoreGive(g_state.in_mutex);
    return -1;
  }

  TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms + 500);
  if (xSemaphoreTake(g_state.in_transfer_done, wait_ticks) != pdTRUE) {
    g_state.in_transfer_in_flight = false;
    xSemaphoreGive(g_state.in_mutex);
    return -1;
  }

  int result = -1;
  if (g_state.last_in_transfer_status == USB_TRANSFER_STATUS_COMPLETED &&
      g_state.last_in_actual_num_bytes > 0) {
    const size_t n = static_cast<size_t>(g_state.last_in_actual_num_bytes);
    const size_t copy_len = n < max_length ? n : max_length;
    memcpy(data, transfer->data_buffer, copy_len);
    result = static_cast<int>(copy_len);
  } else if (g_state.last_in_transfer_status == USB_TRANSFER_STATUS_TIMED_OUT) {
    result = 0;
  }

  xSemaphoreGive(g_state.in_mutex);
  return result;
}

void get_status(UsbPrinterBridgeStatus *status) {
  if (status == nullptr) {
    return;
  }

  *status = {};
  if (g_state.state_mutex == nullptr) {
    return;
  }

  xSemaphoreTake(g_state.state_mutex, portMAX_DELAY);
  status->host_running = g_state.host_running;
  status->device_connected = g_state.device_connected;
  status->printer_ready = g_state.printer_ready;
  status->backend_faulted = g_state.backend_faulted;
  status->dry_run_mode = g_state.dry_run_mode;
  status->device_address = g_state.device_address;
  status->vendor_id = g_state.vendor_id;
  status->product_id = g_state.product_id;
  status->interface_number = g_state.interface_number;
  status->out_endpoint = g_state.out_endpoint;
  status->in_endpoint = g_state.in_endpoint;
  status->total_forwarded_bytes = g_state.total_forwarded_bytes;
  status->total_dropped_bytes = g_state.total_dropped_bytes;
  status->failed_transfer_count = g_state.failed_transfer_count;
  strlcpy(status->last_error, g_state.last_error, sizeof(status->last_error));
  xSemaphoreGive(g_state.state_mutex);
}

const char *last_error() { return g_state.last_error; }

}  // namespace usb_printer_bridge
