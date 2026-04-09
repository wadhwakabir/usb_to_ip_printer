#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <ctype.h>
extern "C" {
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
}

#include "network_config.h"
#include "ring_buffer.h"
#include "usb_printer_bridge.h"

#ifndef RGB_LED_PIN
#define RGB_LED_PIN 48
#endif

#ifndef RGB_LED_COUNT
#define RGB_LED_COUNT 1
#endif

Adafruit_NeoPixel rgbLed(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

namespace {

constexpr uint16_t kRawPrintPort = 9100;
constexpr uint16_t kDebugPort = 2323;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kClientProbeTimeoutMs = 5000;
constexpr uint32_t kPrintIdleTimeoutMs = 30000;
constexpr uint32_t kChunkBufferSize = 4096;
constexpr size_t kDrainChunkSize = 1024;
constexpr size_t kDebugCommandMax = 96;
constexpr size_t kDebugBytesPerPoll = 64;
constexpr size_t kPrintBufferCapacity = 512 * 1024;  // 512KB read-ahead buffer (PSRAM)
constexpr size_t kHeapFallbackCapacity = 32 * 1024;   // conservative heap-only fallback
constexpr size_t kHeapReserveBytes = 64 * 1024;       // minimum free heap to preserve
constexpr uint32_t kBufferFlushTimeoutMs = 30000;
constexpr size_t kPrefillBytes = 64 * 1024;           // pre-fill before USB drain starts
constexpr uint32_t kPrefillTimeoutMs = 2000;           // max wait for pre-fill

enum class DeviceState {
  Booting,
  WifiConnecting,
  AccessPointReady,
  WaitingForPrinter,
  Ready,
  ClientConnected,
  Printing,
  JobComplete,
  Error,
};

struct JobStats {
  bool active = false;
  size_t bytesReceived = 0;
  size_t chunksReceived = 0;
  uint32_t startedAtMs = 0;
  uint32_t lastDataAtMs = 0;
} jobStats;

struct PrintRingBuffer {
  RingBuffer ring;
  SemaphoreHandle_t mutex = nullptr;
  SemaphoreHandle_t data_ready = nullptr;
  TaskHandle_t drain_task = nullptr;
  volatile bool drain_error = false;
  volatile bool job_active = false;
  volatile uint32_t job_generation = 0;
  volatile bool prefill_done = false;
  volatile uint32_t prefill_start_ms = 0;
} printBuf;

// Static TCP read buffer — avoids 4KB stack allocations in the main loop
// that could overflow the Arduino task's 8KB stack.
uint8_t tcpReadBuf[kChunkBufferSize];

DeviceState currentState = DeviceState::Booting;
WiFiServer printServer(kRawPrintPort);
WiFiServer debugServer(kDebugPort);
WiFiClient activeClient;
WiFiClient debugClient;
bool startedAsAccessPoint = false;
uint32_t stateChangedAtMs = 0;
uint32_t lastHeartbeatAtMs = 0;
uint32_t lastWifiLogAtMs = 0;
uint32_t lastProgressLogAtMs = 0;
uint32_t bootedAtMs = 0;
char debugCommandBuffer[kDebugCommandMax] = {0};
size_t debugCommandLength = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
esp_reset_reason_t lastResetReason = ESP_RST_UNKNOWN;

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:
      return "unknown";
    case ESP_RST_POWERON:
      return "power_on";
    case ESP_RST_EXT:
      return "external_pin";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "other_wdt";
    case ESP_RST_DEEPSLEEP:
      return "deep_sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "other";
  }
}

uint32_t ledColor(uint8_t red, uint8_t green, uint8_t blue) {
  return rgbLed.Color(red, green, blue);
}

void writeLed(uint32_t color) {
  rgbLed.setPixelColor(0, color);
  rgbLed.show();
}

const char *stateName(DeviceState state) {
  switch (state) {
    case DeviceState::Booting:
      return "booting";
    case DeviceState::WifiConnecting:
      return "wifi_connecting";
    case DeviceState::AccessPointReady:
      return "access_point_ready";
    case DeviceState::WaitingForPrinter:
      return "waiting_for_printer";
    case DeviceState::Ready:
      return "ready";
    case DeviceState::ClientConnected:
      return "client_connected";
    case DeviceState::Printing:
      return "printing";
    case DeviceState::JobComplete:
      return "job_complete";
    case DeviceState::Error:
      return "error";
  }

  return "unknown";
}

void setState(DeviceState newState) {
  if (currentState == newState) {
    return;
  }

  Serial.printf("[STATE] %s -> %s at %lu ms\n", stateName(currentState),
                stateName(newState), millis());
  currentState = newState;
  stateChangedAtMs = millis();
}

void updateLed() {
  const uint32_t now = millis();
  uint32_t color = 0;

  switch (currentState) {
    case DeviceState::Booting:
      color = ledColor(12, 12, 12);
      break;
    case DeviceState::WifiConnecting:
      color = ((now / 250) % 2 == 0) ? ledColor(0, 0, 32) : 0;
      break;
    case DeviceState::AccessPointReady:
      color = ((now / 700) % 2 == 0) ? ledColor(18, 0, 24) : 0;
      break;
    case DeviceState::WaitingForPrinter:
      color = ((now / 400) % 2 == 0) ? ledColor(18, 10, 0) : 0;
      break;
    case DeviceState::Ready:
      color = ledColor(0, 24, 0);
      break;
    case DeviceState::ClientConnected:
      color = ledColor(0, 18, 18);
      break;
    case DeviceState::Printing:
      color = ((now / 150) % 2 == 0) ? ledColor(24, 18, 0) : 0;
      break;
    case DeviceState::JobComplete:
      color = ((now / 120) % 2 == 0) ? ledColor(0, 32, 0) : 0;
      break;
    case DeviceState::Error:
      color = ((now / 120) % 2 == 0) ? ledColor(32, 0, 0) : 0;
      break;
  }

  writeLed(color);
}

void logNetworkSummary() {
  Serial.printf("[NET] Hostname: %s.local\n", PRINTER_HOSTNAME);
  Serial.printf("[NET] RAW print port: %u\n", kRawPrintPort);
  Serial.printf("[NET] Debug TCP port: %u\n", kDebugPort);
  if (startedAsAccessPoint) {
    Serial.printf("[NET] AP SSID: %s\n", WIFI_AP_SSID);
    Serial.printf("[NET] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[NET] Wi-Fi SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("[NET] Station IP: %s\n", WiFi.localIP().toString().c_str());
  }
}

DeviceState idleStateForCurrentHardware() {
  if (usb_printer_bridge::is_ready()) {
    return DeviceState::Ready;
  }

  return startedAsAccessPoint ? DeviceState::AccessPointReady
                              : DeviceState::WaitingForPrinter;
}

bool startMdns() {
  if (!MDNS.begin(PRINTER_HOSTNAME)) {
    Serial.println("mDNS start failed");
    return false;
  }

  MDNS.addService("printer", "tcp", kRawPrintPort);
  MDNS.addService("pdl-datastream", "tcp", kRawPrintPort);
  return true;
}

bool connectToWifiStation() {
  if (strlen(WIFI_STA_SSID) == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);
  setState(DeviceState::WifiConnecting);
  Serial.printf("[WIFI] Connecting to SSID \"%s\"\n", WIFI_STA_SSID);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < kWifiConnectTimeoutMs) {
    if (millis() - lastWifiLogAtMs > 1000) {
      Serial.printf("[WIFI] Waiting... status=%d elapsed=%lu ms\n",
                    WiFi.status(), millis() - startedAt);
      lastWifiLogAtMs = millis();
    }
    updateLed();
    delay(25);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Connect timed out");
    return false;
  }

  startedAsAccessPoint = false;
  WiFi.setAutoReconnect(true);
  Serial.println("[WIFI] Connected to station network (auto-reconnect enabled)");
  return true;
}

bool startAccessPoint() {
  WiFi.mode(WIFI_AP);
  const bool started = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
  if (!started) {
    Serial.println("[WIFI] SoftAP start failed");
    return false;
  }

  startedAsAccessPoint = true;
  Serial.printf("[WIFI] Started fallback access point \"%s\"\n", WIFI_AP_SSID);
  return true;
}

void startPrintServer() {
  printServer.begin();
  printServer.setNoDelay(true);
  Serial.printf("[SERVER] RAW TCP print server listening on port %u\n",
                kRawPrintPort);
}

void startDebugServer() {
  debugServer.begin();
  debugServer.setNoDelay(true);
  Serial.printf("[SERVER] TCP debug console listening on port %u\n",
                kDebugPort);
}

// --- Print read-ahead buffer: decouples TCP receive from USB send ---
// The main loop writes TCP data in, a FreeRTOS drain task sends it to USB.
// This eliminates printer starvation caused by network round-trip latency.

size_t bufferUsedLocked() { return printBuf.ring.used(); }

size_t bufferFreeLocked() { return printBuf.ring.free_space(); }

size_t bufferWrite(const uint8_t *data, size_t len) {
  xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
  const size_t n = printBuf.ring.write(data, len);
  xSemaphoreGive(printBuf.mutex);
  if (n > 0) {
    xSemaphoreGive(printBuf.data_ready);
  }
  return n;
}

size_t bufferRead(uint8_t *data, size_t maxLen) {
  xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
  const size_t n = printBuf.ring.read(data, maxLen);
  xSemaphoreGive(printBuf.mutex);
  return n;
}

void bufferReset() {
  xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
  printBuf.ring.reset();
  printBuf.drain_error = false;
  xSemaphoreGive(printBuf.mutex);
  xSemaphoreTake(printBuf.data_ready, 0);
}

size_t bufferUsed() {
  xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
  const size_t used = printBuf.ring.used();
  xSemaphoreGive(printBuf.mutex);
  return used;
}

bool initPrintBuffer() {
  // Prefer PSRAM for the full-size buffer
  printBuf.ring.storage =
      static_cast<uint8_t *>(ps_malloc(kPrintBufferCapacity));
  if (printBuf.ring.storage != nullptr) {
    printBuf.ring.capacity = kPrintBufferCapacity;
  } else {
    // PSRAM unavailable — use a small heap buffer that leaves room for
    // WiFi, TCP, and other runtime allocations.
    const size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < kHeapReserveBytes + kHeapFallbackCapacity) {
      Serial.printf("[BUFFER] Not enough heap for print buffer (free=%u, "
                    "need=%u)\n",
                    static_cast<unsigned>(freeHeap),
                    static_cast<unsigned>(kHeapReserveBytes +
                                          kHeapFallbackCapacity));
      return false;
    }
    printBuf.ring.storage =
        static_cast<uint8_t *>(malloc(kHeapFallbackCapacity));
    if (printBuf.ring.storage == nullptr) {
      return false;
    }
    printBuf.ring.capacity = kHeapFallbackCapacity;
    Serial.printf("[BUFFER] PSRAM unavailable, using %uKB heap fallback\n",
                  static_cast<unsigned>(kHeapFallbackCapacity / 1024));
  }
  printBuf.mutex = xSemaphoreCreateMutex();
  printBuf.data_ready = xSemaphoreCreateBinary();
  return printBuf.mutex != nullptr && printBuf.data_ready != nullptr;
}

void printBufferDrainTask(void *) {
  uint8_t chunk[kDrainChunkSize];
  for (;;) {
    xSemaphoreTake(printBuf.data_ready, pdMS_TO_TICKS(10));
    const uint32_t gen = printBuf.job_generation;

    // Pre-fill gate: accumulate data before starting USB output so the
    // printer receives a steady stream instead of starving on WiFi jitter.
    // Adapts to actual buffer size — if PSRAM was unavailable the buffer
    // may be much smaller than kPrefillBytes.
    if (printBuf.job_active && !printBuf.prefill_done) {
      const size_t buffered = bufferUsed();
      const uint32_t elapsed = millis() - printBuf.prefill_start_ms;
      const size_t usable =
          printBuf.ring.capacity > 0 ? printBuf.ring.capacity - 1 : 0;
      const size_t effectivePrefill = min(kPrefillBytes, usable / 2);
      // Start draining when: prefill reached, timeout, or buffer >75% full
      if (buffered < effectivePrefill && elapsed < kPrefillTimeoutMs &&
          buffered < usable * 3 / 4) {
        continue;
      }
      printBuf.prefill_done = true;
      Serial.printf("[BUFFER] Pre-fill %s (%u bytes in %lu ms)\n",
                    buffered >= effectivePrefill ? "reached" : "timed out",
                    static_cast<unsigned>(buffered), elapsed);
    }

    while (printBuf.job_active && !printBuf.drain_error) {
      const size_t n = bufferRead(chunk, sizeof(chunk));
      if (n == 0) {
        break;
      }
      if (!usb_printer_bridge::send_raw(chunk, n)) {
        // Only report error if still the same job — a stale failure from
        // a previous job must not poison the next one.
        if (printBuf.job_generation == gen) {
          printBuf.drain_error = true;
          Serial.printf("[BUFFER] USB drain failed: %s\n",
                        usb_printer_bridge::last_error());
        }
      }
    }
  }
}

void flushPrintBuffer() {
  printBuf.prefill_done = true;  // force drain even if below watermark
  const uint32_t start = millis();
  while (millis() - start < kBufferFlushTimeoutMs) {
    if (printBuf.drain_error) {
      break;
    }
    const size_t remaining = bufferUsed();
    if (remaining == 0) {
      break;
    }
    xSemaphoreGive(printBuf.data_ready);
    delay(10);
  }
  const size_t leftover = bufferUsed();
  if (leftover > 0) {
    Serial.printf("[BUFFER] Flush ended with %u bytes undelivered\n",
                  static_cast<unsigned>(leftover));
  }
}

void beginJob(WiFiClient &client) {
  bufferReset();
  printBuf.job_generation++;
  printBuf.job_active = true;
  printBuf.prefill_done = false;
  printBuf.prefill_start_ms = millis();

  client.setNoDelay(true);

  jobStats.active = true;
  jobStats.bytesReceived = 0;
  jobStats.chunksReceived = 0;
  jobStats.startedAtMs = millis();
  jobStats.lastDataAtMs = jobStats.startedAtMs;
  lastProgressLogAtMs = 0;

  setState(DeviceState::ClientConnected);
  Serial.printf("[CLIENT] Connected from %s:%u\n",
                client.remoteIP().toString().c_str(), client.remotePort());
}

void finishJob(const char *reason, bool markComplete = true) {
  if (!jobStats.active) {
    return;
  }

  if (markComplete && !printBuf.drain_error) {
    flushPrintBuffer();
  }
  printBuf.job_active = false;
  bufferReset();

  const uint32_t elapsed = millis() - jobStats.startedAtMs;
  Serial.printf("[JOB] Finished (%s): %u bytes in %u chunks over %lu ms\n",
                reason, static_cast<unsigned>(jobStats.bytesReceived),
                static_cast<unsigned>(jobStats.chunksReceived), elapsed);
  Serial.printf("[JOB] USB backend status after job: %s\n",
                usb_printer_bridge::last_error());

  jobStats.active = false;
  if (markComplete) {
    setState(DeviceState::JobComplete);
  }
}

void pollForClients() {
  if (!activeClient || !activeClient.connected()) {
    if (jobStats.active) {
      // Drain any residual TCP data into the ring buffer before finishing.
      // connected() usually stays true while available()>0, but a safety
      // drain here closes edge cases around simultaneous data+FIN arrival.
      // Check free space BEFORE reading so we never pull bytes from TCP
      // that we cannot store.
      if (activeClient) {
        while (activeClient.available() > 0) {
          xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
          const size_t free = bufferFreeLocked();
          xSemaphoreGive(printBuf.mutex);
          if (free == 0) break;
          const size_t toRead = min(sizeof(tcpReadBuf), free);
          const int n = activeClient.read(tcpReadBuf, toRead);
          if (n <= 0) break;
          bufferWrite(tcpReadBuf, static_cast<size_t>(n));
          jobStats.bytesReceived += static_cast<size_t>(n);
        }
      }
      finishJob("client disconnected", jobStats.bytesReceived > 0);
    }

    activeClient.stop();
    WiFiClient nextClient = printServer.available();
    if (nextClient) {
      Serial.printf("[CLIENT] Incoming connection from %s:%u while usb_ready=%s "
                    "usb_device=%s\n",
                    nextClient.remoteIP().toString().c_str(),
                    nextClient.remotePort(),
                    usb_printer_bridge::is_ready() ? "yes" : "no",
                    usb_printer_bridge::has_device() ? "yes" : "no");
      activeClient = nextClient;
      beginJob(activeClient);
    }
    return;
  }

  WiFiClient extraClient = printServer.available();
  if (extraClient) {
    Serial.printf("[CLIENT] Rejected extra client %s:%u while busy\n",
                  extraClient.remoteIP().toString().c_str(),
                  extraClient.remotePort());
    extraClient.println("BUSY");
    extraClient.stop();
  }
}

void processPrintStream() {
  if (!activeClient || !activeClient.connected()) {
    return;
  }

  // Check if the drain task hit a USB error
  if (printBuf.drain_error) {
    Serial.printf("[ERROR] Buffer drain failed: %s\n",
                  usb_printer_bridge::last_error());
    setState(DeviceState::Error);
    activeClient.stop();
    finishJob("buffer drain failed", false);
    return;
  }

  bool sawPayload = false;

  while (activeClient.available() > 0) {
    // Check how much buffer space is free before reading from TCP
    xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
    const size_t free = bufferFreeLocked();
    xSemaphoreGive(printBuf.mutex);

    if (free == 0) {
      break;  // Buffer full, let drain task catch up
    }

    const size_t toRead = min(sizeof(tcpReadBuf), free);
    const int bytesRead = activeClient.read(tcpReadBuf, toRead);
    if (bytesRead <= 0) {
      break;
    }

    bufferWrite(tcpReadBuf, static_cast<size_t>(bytesRead));

    sawPayload = true;
    jobStats.bytesReceived += static_cast<size_t>(bytesRead);
    jobStats.chunksReceived += 1;
    jobStats.lastDataAtMs = millis();
  }

  if (sawPayload) {
    if (currentState != DeviceState::Printing) {
      Serial.println("[JOB] Receiving RAW print data");
    }
    setState(DeviceState::Printing);

    if (millis() - lastProgressLogAtMs > 1000) {
      const size_t buffered = bufferUsed();
      Serial.printf("[JOB] Progress: %u bytes received, %u buffered, %u chunks\n",
                    static_cast<unsigned>(jobStats.bytesReceived),
                    static_cast<unsigned>(buffered),
                    static_cast<unsigned>(jobStats.chunksReceived));
      lastProgressLogAtMs = millis();
    }
  }

  const uint32_t idleTimeoutMs =
      jobStats.bytesReceived > 0 ? kPrintIdleTimeoutMs : kClientProbeTimeoutMs;
  if (jobStats.active && millis() - jobStats.lastDataAtMs > idleTimeoutMs) {
    // Don't timeout while buffer still has data being drained to USB
    if (bufferUsed() > 0 && !printBuf.drain_error) {
      return;
    }
    finishJob(jobStats.bytesReceived > 0 ? "idle timeout" : "probe timeout",
              jobStats.bytesReceived > 0);
    activeClient.stop();
  }
}

void restoreReadyStateIfNeeded() {
  if (currentState == DeviceState::JobComplete &&
      millis() - stateChangedAtMs > 1200) {
    setState(idleStateForCurrentHardware());
  }

  if (currentState == DeviceState::ClientConnected &&
      millis() - stateChangedAtMs > 1200) {
    setState(idleStateForCurrentHardware());
  }

  if (currentState == DeviceState::Error &&
      millis() - stateChangedAtMs > 1500 &&
      !usb_printer_bridge::is_faulted()) {
    setState(idleStateForCurrentHardware());
  }
}

void logHeartbeat() {
  if (millis() - lastHeartbeatAtMs < 5000) {
    return;
  }

  lastHeartbeatAtMs = millis();
  UsbPrinterBridgeStatus usb{};
  usb_printer_bridge::get_status(&usb);

  const size_t buf_used = bufferUsed();

  if (startedAsAccessPoint) {
    Serial.printf("[HEARTBEAT] state=%s mode=AP ip=%s clients=%u usb_device=%s "
                  "usb_ready=%s vid=%04x pid=%04x out=0x%02x fwd=%u drop=%u "
                  "fail=%u buf=%u/%uK\n",
                  stateName(currentState), WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPgetStationNum(),
                  usb.device_connected ? "yes" : "no",
                  usb.printer_ready ? "yes" : "no", usb.vendor_id,
                  usb.product_id, usb.out_endpoint,
                  static_cast<unsigned>(usb.total_forwarded_bytes),
                  static_cast<unsigned>(usb.total_dropped_bytes),
                  static_cast<unsigned>(usb.failed_transfer_count),
                  static_cast<unsigned>(buf_used),
                  static_cast<unsigned>(printBuf.ring.capacity / 1024));
    return;
  }

  Serial.printf("[HEARTBEAT] state=%s mode=STA ip=%s rssi=%d usb_device=%s "
                "usb_ready=%s vid=%04x pid=%04x out=0x%02x fwd=%u drop=%u "
                "fail=%u buf=%u/%uK heap=%u\n",
                stateName(currentState), WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), usb.device_connected ? "yes" : "no",
                usb.printer_ready ? "yes" : "no", usb.vendor_id,
                usb.product_id, usb.out_endpoint,
                static_cast<unsigned>(usb.total_forwarded_bytes),
                static_cast<unsigned>(usb.total_dropped_bytes),
                static_cast<unsigned>(usb.failed_transfer_count),
                static_cast<unsigned>(buf_used),
                static_cast<unsigned>(printBuf.ring.capacity / 1024),
                ESP.getFreeHeap());
}

void writeDebugPrompt(WiFiClient &client) { client.print(F("esp32-print> ")); }

void writeDebugStatus(WiFiClient &client) {
  UsbPrinterBridgeStatus usb{};
  usb_printer_bridge::get_status(&usb);

  const String ip = startedAsAccessPoint ? WiFi.softAPIP().toString()
                                         : WiFi.localIP().toString();
  const String ssid = startedAsAccessPoint ? String(WIFI_AP_SSID) : WiFi.SSID();

  client.print(F("state="));
  client.println(stateName(currentState));
  client.print(F("mode="));
  client.println(startedAsAccessPoint ? F("AP") : F("STA"));
  client.print(F("ip="));
  client.println(ip);
  client.print(F("ssid="));
  client.println(ssid);
  client.print(F("raw_port="));
  client.println(kRawPrintPort);
  client.print(F("debug_port="));
  client.println(kDebugPort);
  client.print(F("uptime_ms="));
  client.println(millis() - bootedAtMs);
  client.print(F("boot_count="));
  client.println(bootCount);
  client.print(F("reset_reason="));
  client.println(resetReasonName(lastResetReason));
  client.print(F("free_heap_bytes="));
  client.println(ESP.getFreeHeap());
  client.print(F("min_free_heap_bytes="));
  client.println(ESP.getMinFreeHeap());
  client.print(F("job_active="));
  client.println(jobStats.active ? F("true") : F("false"));
  client.print(F("job_bytes_received="));
  client.println(jobStats.bytesReceived);
  client.print(F("job_chunks_received="));
  client.println(jobStats.chunksReceived);
  client.print(F("usb_host_running="));
  client.println(usb.host_running ? F("true") : F("false"));
  client.print(F("usb_device_connected="));
  client.println(usb.device_connected ? F("true") : F("false"));
  client.print(F("usb_printer_ready="));
  client.println(usb.printer_ready ? F("true") : F("false"));
  client.print(F("usb_backend_faulted="));
  client.println(usb.backend_faulted ? F("true") : F("false"));
  client.print(F("usb_vendor_id="));
  client.printf("%04x\n", usb.vendor_id);
  client.print(F("usb_product_id="));
  client.printf("%04x\n", usb.product_id);
  client.print(F("usb_interface_number="));
  client.println(usb.interface_number);
  client.print(F("usb_out_endpoint="));
  client.println(usb.out_endpoint);
  client.print(F("usb_in_endpoint="));
  client.println(usb.in_endpoint);
  client.print(F("usb_total_forwarded_bytes="));
  client.println(usb.total_forwarded_bytes);
  client.print(F("usb_total_dropped_bytes="));
  client.println(usb.total_dropped_bytes);
  client.print(F("usb_failed_transfer_count="));
  client.println(usb.failed_transfer_count);
  client.print(F("usb_last_error="));
  client.println(usb.last_error);
  client.print(F("buffer_used_bytes="));
  client.println(bufferUsed());
  client.print(F("buffer_capacity_bytes="));
  client.println(printBuf.ring.capacity);
  client.print(F("buffer_drain_error="));
  client.println(printBuf.drain_error ? F("true") : F("false"));
}

void writeDebugHelp(WiFiClient &client) {
  client.println(F("Commands:"));
  client.println(F("  help   - show this help"));
  client.println(F("  status - full bridge status"));
  client.println(F("  usb    - USB counters and last error"));
  client.println(F("  job    - current job counters"));
  client.println(F("  buf    - print buffer status"));
  client.println(F("  heap   - heap and reset info"));
  client.println(F("  close  - close this debug session"));
}

void writeUsbSummary(WiFiClient &client) {
  UsbPrinterBridgeStatus usb{};
  usb_printer_bridge::get_status(&usb);

  client.printf("ready=%s device=%s faulted=%s vid=%04x pid=%04x out=0x%02x in=0x%02x\n",
                usb.printer_ready ? "yes" : "no",
                usb.device_connected ? "yes" : "no",
                usb.backend_faulted ? "yes" : "no", usb.vendor_id,
                usb.product_id, usb.out_endpoint, usb.in_endpoint);
  client.printf("forwarded=%u dropped=%u failed=%u\n",
                static_cast<unsigned>(usb.total_forwarded_bytes),
                static_cast<unsigned>(usb.total_dropped_bytes),
                static_cast<unsigned>(usb.failed_transfer_count));
  client.print(F("last_error="));
  client.println(usb.last_error);
}

void writeJobSummary(WiFiClient &client) {
  client.printf("active=%s bytes=%u chunks=%u state=%s\n",
                jobStats.active ? "yes" : "no",
                static_cast<unsigned>(jobStats.bytesReceived),
                static_cast<unsigned>(jobStats.chunksReceived),
                stateName(currentState));
}

void writeHeapSummary(WiFiClient &client) {
  client.printf("free_heap=%u min_free_heap=%u uptime_ms=%lu boot_count=%lu reset_reason=%s\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(), millis() - bootedAtMs,
                bootCount, resetReasonName(lastResetReason));
}

void handleDebugCommand(WiFiClient &client, const char *rawCommand) {
  char command[kDebugCommandMax] = {0};
  size_t length = 0;
  while (rawCommand[length] != '\0' && length + 1 < sizeof(command)) {
    command[length] = static_cast<char>(tolower(rawCommand[length]));
    length += 1;
  }
  command[length] = '\0';

  while (length > 0 &&
         (command[length - 1] == ' ' || command[length - 1] == '\t')) {
    command[--length] = '\0';
  }

  char *start = command;
  while (*start == ' ' || *start == '\t') {
    start += 1;
  }

  if (*start == '\0') {
    return;
  }

  if (strcmp(start, "help") == 0) {
    writeDebugHelp(client);
    return;
  }
  if (strcmp(start, "status") == 0) {
    writeDebugStatus(client);
    return;
  }
  if (strcmp(start, "usb") == 0) {
    writeUsbSummary(client);
    return;
  }
  if (strcmp(start, "job") == 0) {
    writeJobSummary(client);
    return;
  }
  if (strcmp(start, "buf") == 0) {
    const size_t used = bufferUsed();
    client.printf("used=%u capacity=%u drain_error=%s job_active=%s\n",
                  static_cast<unsigned>(used),
                  static_cast<unsigned>(printBuf.ring.capacity),
                  printBuf.drain_error ? "yes" : "no",
                  printBuf.job_active ? "yes" : "no");
    return;
  }
  if (strcmp(start, "heap") == 0) {
    writeHeapSummary(client);
    return;
  }
  if (strcmp(start, "close") == 0 || strcmp(start, "exit") == 0 ||
      strcmp(start, "quit") == 0) {
    client.println(F("Closing debug session."));
    client.stop();
    return;
  }

  client.print(F("Unknown command: "));
  client.println(start);
  writeDebugHelp(client);
}

void pollDebugServer() {
  if (!debugClient || !debugClient.connected()) {
    if (debugClient) {
      debugClient.stop();
    }

    WiFiClient nextClient = debugServer.available();
    if (!nextClient) {
      return;
    }

    debugClient = nextClient;
    debugClient.setNoDelay(true);
    debugCommandLength = 0;
    Serial.printf("[DEBUG] Console connected from %s:%u\n",
                  debugClient.remoteIP().toString().c_str(),
                  debugClient.remotePort());
    debugClient.println(F("ESP32 Print Bridge Debug Console"));
    debugClient.println(F("Type 'help' for commands."));
    writeDebugPrompt(debugClient);
    return;
  }

  WiFiClient extraClient = debugServer.available();
  if (extraClient) {
    extraClient.println(F("BUSY"));
    extraClient.stop();
  }

  size_t bytesProcessed = 0;
  while (debugClient.available() > 0 && bytesProcessed < kDebugBytesPerPoll) {
    const char c = static_cast<char>(debugClient.read());
    bytesProcessed += 1;
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      debugCommandBuffer[debugCommandLength] = '\0';
      handleDebugCommand(debugClient, debugCommandBuffer);
      if (debugClient && debugClient.connected()) {
        writeDebugPrompt(debugClient);
      }
      debugCommandLength = 0;
      continue;
    }
    if (debugCommandLength + 1 >= sizeof(debugCommandBuffer)) {
      debugClient.println(F("Command too long."));
      debugCommandLength = 0;
      writeDebugPrompt(debugClient);
      continue;
    }
    debugCommandBuffer[debugCommandLength++] = c;
  }
}

void checkWifiConnection() {
  if (startedAsAccessPoint) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    // If we were showing WifiConnecting, restore idle state now
    if (currentState == DeviceState::WifiConnecting) {
      Serial.printf("[WIFI] Reconnected, IP=%s\n",
                    WiFi.localIP().toString().c_str());
      setState(idleStateForCurrentHardware());
    }
    return;
  }
  // Wi-Fi dropped — show it in state unless a job is active (auto-reconnect
  // handles the actual reconnection in the background)
  if (!jobStats.active &&
      currentState != DeviceState::WifiConnecting &&
      currentState != DeviceState::Error) {
    Serial.println("[WIFI] Connection lost, waiting for auto-reconnect");
    setState(DeviceState::WifiConnecting);
  }
}

void syncIdleStateWithPrinter() {
  if (currentState == DeviceState::Booting ||
      currentState == DeviceState::WifiConnecting ||
      currentState == DeviceState::Printing ||
      currentState == DeviceState::ClientConnected ||
      currentState == DeviceState::JobComplete ||
      currentState == DeviceState::Error) {
    return;
  }

  const DeviceState desired = idleStateForCurrentHardware();
  if (currentState != desired) {
    setState(desired);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);
  bootCount += 1;
  lastResetReason = esp_reset_reason();
  bootedAtMs = millis();

  rgbLed.begin();
  rgbLed.setBrightness(32);
  rgbLed.clear();
  rgbLed.show();
  setState(DeviceState::Booting);
  updateLed();

  Serial.println();
  Serial.println("ESP32-S3 RAW TCP Print Server");
  Serial.printf("[BOOT] Boot count: %lu\n", bootCount);
  Serial.printf("[BOOT] Reset reason: %s\n", resetReasonName(lastResetReason));
  Serial.printf("[BOOT] CPU frequency: %lu MHz\n", getCpuFrequencyMhz());

  if (psramInit()) {
    Serial.printf("[BOOT] PSRAM detected: %u bytes\n", ESP.getPsramSize());
  } else {
    Serial.println("[BOOT] PSRAM not detected");
  }

  Serial.printf("[BOOT] RGB LED on GPIO %d\n", RGB_LED_PIN);

  const bool connected = connectToWifiStation();
  if (!connected && !startAccessPoint()) {
    Serial.println("[ERROR] Failed to bring up any network mode");
    setState(DeviceState::Error);
    return;
  }

  if (!startMdns()) {
    Serial.println("[WARN] Continuing without mDNS");
  } else {
    Serial.printf("[MDNS] Services advertised as %s.local\n",
                  PRINTER_HOSTNAME);
  }

  if (!usb_printer_bridge::begin()) {
    Serial.printf("[ERROR] USB printer bridge init failed: %s\n",
                  usb_printer_bridge::last_error());
    setState(DeviceState::Error);
    return;
  }

  if (!initPrintBuffer()) {
    Serial.println("[ERROR] Print buffer allocation failed");
    setState(DeviceState::Error);
    return;
  }
  Serial.printf("[BUFFER] %uKB read-ahead buffer allocated (%s, heap_free=%u)\n",
                static_cast<unsigned>(printBuf.ring.capacity / 1024),
                printBuf.ring.storage >= (uint8_t *)0x3C000000 ? "PSRAM"
                                                               : "heap",
                ESP.getFreeHeap());
  xTaskCreatePinnedToCore(printBufferDrainTask, "buf_drain", 4096, nullptr, 10,
                          &printBuf.drain_task, 0);
  if (printBuf.drain_task == nullptr) {
    Serial.println("[ERROR] Buffer drain task creation failed");
    setState(DeviceState::Error);
    return;
  }

  startPrintServer();
  startDebugServer();
  logNetworkSummary();
  Serial.println("[USB] Waiting for a USB printer on the ESP32-S3 host port");
  setState(idleStateForCurrentHardware());
}

void loop() {
  updateLed();

  pollForClients();
  pollDebugServer();
  processPrintStream();
  restoreReadyStateIfNeeded();
  syncIdleStateWithPrinter();
  checkWifiConnection();
  logHeartbeat();
  delay(currentState == DeviceState::Printing ? 1 : 5);
}
