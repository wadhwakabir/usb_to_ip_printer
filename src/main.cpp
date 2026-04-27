#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ctype.h>
extern "C" {
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
}

#include "ring_buffer.h"
#include "usb_printer_bridge.h"

// Network identity is baked into the firmware so every unit ships identical
// and the setup flow is the same everywhere.  Wi-Fi station credentials are
// entered by the end user via the captive portal and stored in NVS — there is
// no compile-time STA config.
#define WIFI_AP_SSID      "esp32-printer"
#define WIFI_AP_PASSWORD  "printprint"
#define PRINTER_HOSTNAME  "esp32-printer"

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
constexpr size_t kDrainChunkSize = 4096;
constexpr size_t kDebugCommandMax = 96;
constexpr size_t kDebugBytesPerPoll = 64;
constexpr size_t kPrintBufferCapacity = 512 * 1024;  // 512KB read-ahead buffer (PSRAM)
constexpr size_t kHeapFallbackCapacity = 32 * 1024;   // conservative heap-only fallback
constexpr size_t kHeapReserveBytes = 64 * 1024;       // minimum free heap to preserve
constexpr uint32_t kBufferFlushTimeoutMs = 10000;
constexpr uint32_t kRecvQuiesceTimeoutMs = 250;
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
  TaskHandle_t recv_task = nullptr;
  volatile bool drain_error = false;
  volatile bool job_active = false;
  volatile uint32_t job_generation = 0;
  volatile bool prefill_done = false;
  volatile uint32_t prefill_start_ms = 0;
  volatile bool recv_active = false;  // gates usbRecvTask access to activeClient
  volatile bool recv_in_flight = false;  // set while recv task is inside activeClient.write()
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
uint32_t bootedAtMs = 0;
char debugCommandBuffer[kDebugCommandMax] = {0};
size_t debugCommandLength = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
esp_reset_reason_t lastResetReason = ESP_RST_UNKNOWN;

// --- Wi-Fi provisioning (captive portal) ---------------------------------
// Wi-Fi STA credentials are stored in NVS and entered at runtime through the
// captive portal.  The flow:
//   boot -> load NVS -> try STA -> on failure start AP + captive portal
//   user connects to AP, fills form -> save to NVS -> reboot -> try STA
// On moving houses the STA connect fails and the device drops back into
// AP/captive mode automatically, no reset command needed.
constexpr uint16_t kDnsPort = 53;
constexpr uint16_t kHttpPort = 80;
constexpr const char *kPrefsNamespace = "wifi";
constexpr const char *kPrefsKeySsid = "ssid";
constexpr const char *kPrefsKeyPass = "pass";

Preferences wifiPrefs;
DNSServer dnsServer;
WebServer httpServer(kHttpPort);
bool captivePortalActive = false;
char provisionedSsid[64] = {0};
char provisionedPass[64] = {0};

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
  // While the captive portal is up, the device is not usable for printing
  // regardless of USB state — show AccessPointReady so a non-technical user
  // doesn't mistake a solid green LED for "configured" when we're still
  // waiting on Wi-Fi credentials.
  if (captivePortalActive) {
    return DeviceState::AccessPointReady;
  }
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

void loadProvisionedCredentials() {
  wifiPrefs.begin(kPrefsNamespace, /*readOnly=*/true);
  const String s = wifiPrefs.getString(kPrefsKeySsid, "");
  const String p = wifiPrefs.getString(kPrefsKeyPass, "");
  wifiPrefs.end();
  strlcpy(provisionedSsid, s.c_str(), sizeof(provisionedSsid));
  strlcpy(provisionedPass, p.c_str(), sizeof(provisionedPass));
}

bool saveProvisionedCredentials(const char *ssid, const char *pass) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }
  wifiPrefs.begin(kPrefsNamespace, /*readOnly=*/false);
  wifiPrefs.putString(kPrefsKeySsid, ssid);
  wifiPrefs.putString(kPrefsKeyPass, pass ? pass : "");
  wifiPrefs.end();
  return true;
}

bool connectToWifiStation() {
  // Credentials always come from NVS (provisioned via the captive portal).
  // No compile-time STA config: every unit is identical until the end user
  // configures it.
  if (provisionedSsid[0] == '\0') {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(provisionedSsid, provisionedPass);
  setState(DeviceState::WifiConnecting);
  Serial.printf("[WIFI] Connecting to SSID \"%s\"\n", provisionedSsid);

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
    WiFi.disconnect(true, /*eraseap=*/false);
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

// --- Captive portal ------------------------------------------------------

// Minimal HTML escaper for user-supplied SSID echoed back into the form.
String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String renderPortalPage(const String &message) {
  String scanList;
  const int n = WiFi.scanComplete();
  if (n > 0) {
    scanList += F("<datalist id=\"nets\">");
    for (int i = 0; i < n; ++i) {
      scanList += F("<option value=\"");
      scanList += htmlEscape(WiFi.SSID(i));
      scanList += F("\">");
    }
    scanList += F("</datalist>");
  }

  String page;
  page.reserve(1800);
  page += F(
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Printer setup</title>"
      "<style>body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;"
      "max-width:420px;margin:2em auto;padding:0 1em;color:#222}"
      "h1{font-size:1.3em}label{display:block;margin:1em 0 .3em}"
      "input{width:100%;padding:.6em;font-size:1em;box-sizing:border-box;"
      "border:1px solid #bbb;border-radius:6px}"
      "button{margin-top:1.5em;width:100%;padding:.8em;font-size:1em;"
      "background:#0a7;color:#fff;border:0;border-radius:6px}"
      ".msg{padding:.7em;border-radius:6px;background:#eef;margin:1em 0}"
      ".hint{color:#666;font-size:.9em;margin-top:.3em}</style></head><body>"
      "<h1>Printer Wi-Fi setup</h1>");
  if (message.length() > 0) {
    page += F("<div class=\"msg\">");
    page += message;
    page += F("</div>");
  }
  // Pre-fill the SSID with what's currently saved so a password-only rotation
  // (common when a router's guest/main password changes) doesn't force the
  // user to retype their network name.  Password is never echoed back.
  String ssidValue;
  if (provisionedSsid[0] != '\0') {
    ssidValue = htmlEscape(String(provisionedSsid));
  }

  page += F(
      "<form method=\"POST\" action=\"/save\">"
      "<label>Wi-Fi network name (SSID)"
      "<input name=\"ssid\" list=\"nets\" required autofocus value=\"");
  page += ssidValue;
  page += F(
      "\"></label>"
      "<label>Password"
      "<input name=\"pass\" type=\"password\"></label>"
      "<div class=\"hint\">The printer will reboot and try to join this "
      "network. If it can't, it will re-open this setup page.</div>"
      "<button type=\"submit\">Save &amp; reboot</button></form>");
  page += scanList;
  page += F("</body></html>");
  return page;
}

void handlePortalRoot() {
  // Scan asynchronously so the form gets a fresh list next load without
  // blocking this response.
  if (WiFi.scanComplete() == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(/*async=*/true);
  }
  httpServer.send(200, "text/html", renderPortalPage(""));
}

void handlePortalSave() {
  const String ssid = httpServer.arg("ssid");
  const String pass = httpServer.arg("pass");
  if (ssid.length() == 0 || ssid.length() >= sizeof(provisionedSsid) ||
      pass.length() >= sizeof(provisionedPass)) {
    httpServer.send(400, "text/html",
                    renderPortalPage(F("SSID is required and must be "
                                       "under 64 chars.")));
    return;
  }
  if (!saveProvisionedCredentials(ssid.c_str(), pass.c_str())) {
    httpServer.send(500, "text/html",
                    renderPortalPage(F("Could not save credentials.")));
    return;
  }
  Serial.printf("[PORTAL] Saved credentials for SSID \"%s\", rebooting\n",
                ssid.c_str());
  httpServer.send(200, "text/html",
                  F("<!doctype html><meta charset=\"utf-8\"><body style=\""
                    "font-family:sans-serif;max-width:420px;margin:2em auto\">"
                    "<h1>Saved</h1><p>Rebooting and attempting to join the "
                    "network. If the printer can't connect, it will re-open "
                    "this setup Wi-Fi.</p></body>"));
  delay(400);
  ESP.restart();
}

// Most phones probe a few well-known URLs to detect captive portals.  Serving
// a 302 to the root makes the OS pop up the "Sign in to network" sheet
// automatically, so the non-technical user doesn't have to type an IP.
void handleCaptiveProbe() {
  httpServer.sendHeader("Location", "http://192.168.4.1/", /*first=*/true);
  httpServer.send(302, "text/plain", "");
}

void startCaptivePortal() {
  if (captivePortalActive) {
    return;
  }
  const IPAddress apIp = WiFi.softAPIP();
  // DNS hijack: every lookup resolves to us, so any URL the OS probes lands
  // on our server and triggers the captive portal UX.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(kDnsPort, "*", apIp);

  httpServer.on("/", HTTP_GET, handlePortalRoot);
  httpServer.on("/save", HTTP_POST, handlePortalSave);
  // Catch-alls for common OS captive-portal probes.
  httpServer.on("/generate_204", HTTP_GET, handleCaptiveProbe);        // Android
  httpServer.on("/gen_204", HTTP_GET, handleCaptiveProbe);             // Android
  httpServer.on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe); // iOS/macOS
  httpServer.on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
  httpServer.on("/ncsi.txt", HTTP_GET, handleCaptiveProbe);            // Windows
  httpServer.on("/connecttest.txt", HTTP_GET, handleCaptiveProbe);     // Windows
  httpServer.onNotFound(handleCaptiveProbe);
  httpServer.begin();

  WiFi.scanNetworks(/*async=*/true);  // warm up the SSID list
  captivePortalActive = true;
  Serial.printf("[PORTAL] Captive portal active at http://%s/\n",
                apIp.toString().c_str());
}

void pollCaptivePortal() {
  if (!captivePortalActive) {
    return;
  }
  dnsServer.processNextRequest();
  httpServer.handleClient();
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
  xSemaphoreGive(printBuf.mutex);
  xSemaphoreTake(printBuf.data_ready, 0);
}

void bufferClearDrainError() {
  xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
  printBuf.drain_error = false;
  xSemaphoreGive(printBuf.mutex);
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
        // Buffer momentarily empty — wait for new data without leaving
        // the drain loop.  Breaking out and re-entering via the outer
        // semaphore adds ~10 ms dead time that shows up as stutter.
        xSemaphoreTake(printBuf.data_ready, pdMS_TO_TICKS(1));
        continue;
      }
      if (!usb_printer_bridge::send_raw(chunk, n)) {
        if (printBuf.job_generation == gen) {
          printBuf.drain_error = true;
          Serial.printf("[BUFFER] USB drain failed: %s\n",
                        usb_printer_bridge::last_error());
        }
      }
    }
  }
}

void usbRecvTask(void *) {
  uint8_t buf[512];
  // Tracks how long the TCP send buffer has been too full to accept bytes.
  // A stalled peer (not reading the socket) would otherwise block
  // activeClient.write() indefinitely, wedging this task and causing
  // finishJob()'s quiesce wait to always time out.
  uint32_t stall_started_ms = 0;
  constexpr uint32_t kRecvWriteStallAbortMs = 3000;
  for (;;) {
    // Only run when the main loop has explicitly activated recv for this job.
    // recv_active is set in beginJob() and cleared in finishJob() before
    // activeClient is touched, avoiding race conditions with the main loop.
    if (!printBuf.recv_active || printBuf.drain_error) {
      stall_started_ms = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    const int n = usb_printer_bridge::recv_raw(buf, sizeof(buf), 100);
    if (n > 0) {
      // Guard the activeClient.write with a pair of flags: recv_active is the
      // permission gate, recv_in_flight tells finishJob() to wait for us to
      // exit before it touches activeClient.  The order matters:
      // re-check recv_active AFTER asserting in_flight, so finishJob() either
      // sees in_flight=true (and waits) or sees in_flight=false after we've
      // already bailed out — never a stale activeClient from mid-write.
      printBuf.recv_in_flight = true;
      if (printBuf.recv_active) {
        // Only push what fits in the TCP send buffer right now.  write() on
        // ESP32 Arduino can block when the LwIP send buffer is full; if the
        // peer stopped reading, that block is open-ended and finishJob()'s
        // 250 ms quiesce wait always expires.  Bounding by availableForWrite
        // turns the block into a short loop we can bail out of.
        const int avail = activeClient.availableForWrite();
        if (avail > 0) {
          const size_t to_write =
              static_cast<size_t>(n) < static_cast<size_t>(avail)
                  ? static_cast<size_t>(n)
                  : static_cast<size_t>(avail);
          activeClient.write(buf, to_write);
          stall_started_ms = 0;
        } else {
          // Peer is not draining the socket.  Start / continue the stall
          // timer; abort the socket if it persists, so the main loop sees
          // a disconnect and finishJob() can run cleanly.
          if (stall_started_ms == 0) {
            stall_started_ms = millis();
          } else if (millis() - stall_started_ms > kRecvWriteStallAbortMs) {
            Serial.println("[RECV] TCP send buffer stalled; aborting client");
            activeClient.stop();
            stall_started_ms = 0;
          }
        }
      }
      printBuf.recv_in_flight = false;
    } else if (n < 0) {
      // recv_raw returned an error (device gone, no IN endpoint, etc.).
      // Avoid busy-spinning — pause before retrying.
      stall_started_ms = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
    } else {
      // n == 0: timeout, nothing to write — reset stall tracker.
      stall_started_ms = 0;
    }
  }
}

void pollDebugServer();

void flushPrintBuffer() {
  printBuf.prefill_done = true;  // force drain even if below watermark
  const uint32_t start = millis();
  size_t last_logged_remaining = 0;
  while (millis() - start < kBufferFlushTimeoutMs) {
    if (printBuf.drain_error) {
      break;
    }
    const size_t remaining = bufferUsed();
    if (remaining == 0) {
      break;
    }
    xSemaphoreGive(printBuf.data_ready);

    // Keep the LED alive, service the debug console, and reject stray clients
    // while we wait.  The main loop is blocked here, so without this the
    // device appears frozen for the full flush window if the USB side stalls
    // — which defeats the whole point of having a debug console.
    updateLed();
    pollDebugServer();
    WiFiClient extra = printServer.available();
    if (extra) {
      extra.println("BUSY");
      extra.stop();
    }

    // Periodic progress log so a stuck flush is visible in the monitor.
    if (millis() - start > 2000 &&
        (last_logged_remaining == 0 ||
         (last_logged_remaining > remaining &&
          last_logged_remaining - remaining >= 16 * 1024))) {
      Serial.printf("[BUFFER] Flushing... %u bytes remaining\n",
                    static_cast<unsigned>(remaining));
      last_logged_remaining = remaining;
    }

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
  // Only clear the drain_error flag if USB looks usable again.  If a previous
  // job ended because USB died and the printer hasn't recovered, starting a
  // fresh job with drain_error=false would accept TCP bytes that the drain
  // task immediately fails to deliver — the user would see the job "complete"
  // while nothing printed.  Leaving drain_error=true short-circuits
  // processPrintStream() on the next loop iteration and surfaces the failure.
  if (usb_printer_bridge::is_ready_and_healthy()) {
    bufferClearDrainError();
  } else if (printBuf.drain_error) {
    Serial.println("[JOB] Previous drain error persists — USB not ready yet");
  }
  // Pre-fill fields MUST be reset before job_active flips true: the drain task
  // samples job_active and prefill_done independently, so a stale
  // prefill_done=true from the previous job can race with a new
  // job_active=true and skip the pre-fill wait entirely.
  printBuf.prefill_done = false;
  printBuf.prefill_start_ms = millis();
  printBuf.job_generation++;
  printBuf.recv_active = true;
  printBuf.job_active = true;

  client.setNoDelay(true);

  jobStats.active = true;
  jobStats.bytesReceived = 0;
  jobStats.chunksReceived = 0;
  jobStats.startedAtMs = millis();
  jobStats.lastDataAtMs = jobStats.startedAtMs;
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
  // Stop the recv task before touching activeClient — recv_active gates all
  // activeClient access from the recv task, preventing races with the main loop.
  printBuf.recv_active = false;
  printBuf.job_active = false;
  // Wait for the recv task to exit any in-progress activeClient.write().
  // A fixed delay isn't enough: the task may be mid-write when recv_active
  // is cleared, and activeClient.stop() racing with write() has previously
  // corrupted WiFiClient state.  recv_in_flight is set around the write()
  // call, so this loop observes a true→false transition deterministically.
  const uint32_t quiesce_start = millis();
  while (printBuf.recv_in_flight &&
         millis() - quiesce_start < kRecvQuiesceTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (printBuf.recv_in_flight) {
    Serial.println("[JOB] recv task did not quiesce in time; proceeding anyway");
  }
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
      // processPrintStream() already captured all reachable TCP data while
      // _connected was still true.  connected() above has now poisoned that
      // flag, so available()/read() would return 0 — no point retrying here.
      finishJob("client disconnected", jobStats.bytesReceived > 0);
    }

    activeClient.stop();
    WiFiClient nextClient = printServer.available();
    if (nextClient) {
      // Refuse up-front when the device is in setup mode (captive portal
      // active = still provisioning Wi-Fi).  Anything that connects to :9100
      // on the setup AP is almost certainly a leftover printer queue from
      // the user's previous home, not a real job we can deliver.
      if (captivePortalActive) {
        Serial.printf("[CLIENT] Rejected %s:%u: device is in Wi-Fi setup "
                      "mode\n",
                      nextClient.remoteIP().toString().c_str(),
                      nextClient.remotePort());
        nextClient.println("BUSY wifi setup pending");
        nextClient.stop();
        return;
      }
      // Refuse the connection up-front when a drain error is latched.  The
      // README promises "jobs are rejected until the error is cleared"; if we
      // accept here, the client happily sends data, processPrintStream() sees
      // drain_error and tears down mid-stream, and the client observes an
      // unexplained close with silent data loss.  Rejecting before beginJob()
      // makes the contract match reality: run `clear-error` to print again.
      if (printBuf.drain_error) {
        Serial.printf("[CLIENT] Rejected %s:%u: drain_error latched — "
                      "run `clear-error` on debug console\n",
                      nextClient.remoteIP().toString().c_str(),
                      nextClient.remotePort());
        nextClient.println("BUSY drain_error; run clear-error");
        nextClient.stop();
        return;
      }
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
  // Guard: skip if no client or no active job.
  // IMPORTANT: do NOT call activeClient.connected() here.  On ESP32 Arduino
  // it probes the socket with recv(fd,&dummy,0,...) which can set the
  // internal _connected flag to false when the peer has sent FIN, even when
  // unread data remains in the TCP receive buffer.  Once _connected is
  // false, available() and read() both short-circuit to 0/−1 and the
  // remaining payload is silently lost.  Disconnect detection is handled
  // exclusively by pollForClients() which runs AFTER this function.
  if (!activeClient || !jobStats.active) {
    return;
  }

  // Check if the drain task hit a USB error
  if (printBuf.drain_error) {
    Serial.printf("[ERROR] Buffer drain failed: %s\n",
                  usb_printer_bridge::last_error());
    setState(DeviceState::Error);
    finishJob("buffer drain failed", false);
    activeClient.stop();
    return;
  }

  bool sawPayload = false;

  while (activeClient.available() > 0) {
    // Check how much buffer space is free before reading from TCP
    xSemaphoreTake(printBuf.mutex, portMAX_DELAY);
    const size_t free = printBuf.ring.free_space();
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
  if (currentState == DeviceState::Printing) {
    return;
  }
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
  client.println(F("  help        - show this help"));
  client.println(F("  status      - full bridge status"));
  client.println(F("  net         - network/IP summary"));
  client.println(F("  usb         - USB counters and last error"));
  client.println(F("  job         - current job counters"));
  client.println(F("  buf         - print buffer status"));
  client.println(F("  heap        - heap and reset info"));
  client.println(F("  clear-error - clear a stuck drain error after reattaching printer"));
  client.println(F("  forget-wifi - erase saved Wi-Fi credentials and reboot into setup AP"));
  client.println(F("  reboot      - restart the ESP32 (drops this session)"));
  client.println(F("  close       - close this debug session"));
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

void writeNetSummary(WiFiClient &client) {
  const String ip = startedAsAccessPoint ? WiFi.softAPIP().toString()
                                         : WiFi.localIP().toString();
  const String ssid =
      startedAsAccessPoint ? String(WIFI_AP_SSID) : WiFi.SSID();
  client.printf("mode=%s ssid=%s ip=%s hostname=%s.local raw_port=%u "
                "debug_port=%u\n",
                startedAsAccessPoint ? "AP" : "STA", ssid.c_str(), ip.c_str(),
                PRINTER_HOSTNAME, kRawPrintPort, kDebugPort);
  if (!startedAsAccessPoint) {
    client.printf("rssi=%d wifi_status=%d\n", WiFi.RSSI(), WiFi.status());
  } else {
    client.printf("ap_clients=%u\n", WiFi.softAPgetStationNum());
  }
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
    command[length] = static_cast<char>(
        tolower(static_cast<unsigned char>(rawCommand[length])));
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
  if (strcmp(start, "net") == 0) {
    writeNetSummary(client);
    return;
  }
  if (strcmp(start, "clear-error") == 0) {
    // Manual recovery path: after the user unplugs and re-plugs a printer
    // (or the USB backend recovers from a halt), clear the drain_error flag
    // so the next print job is accepted.  Refuses if a job is currently
    // active — clearing mid-print would confuse backpressure signaling.
    if (printBuf.job_active || jobStats.active) {
      client.println(F("Cannot clear error while a job is active."));
      return;
    }
    if (!printBuf.drain_error) {
      client.println(F("No drain error to clear."));
      return;
    }
    bufferClearDrainError();
    Serial.println("[DEBUG] drain_error cleared via debug console");
    client.println(F("drain_error cleared."));
    return;
  }
  if (strcmp(start, "forget-wifi") == 0) {
    // Wipe NVS-stored credentials and reboot.  Next boot will find no saved
    // SSID and come up as the captive-portal AP so the user can re-provision.
    // Allowed while a job is active — this is a manual recovery command and
    // the user knows they're blowing away the session.
    wifiPrefs.begin(kPrefsNamespace, /*readOnly=*/false);
    wifiPrefs.clear();
    wifiPrefs.end();
    Serial.println("[DEBUG] Wi-Fi credentials erased; rebooting into setup AP");
    client.println(F("Wi-Fi credentials erased. Rebooting into setup AP..."));
    client.stop();
    delay(200);
    ESP.restart();
    return;
  }
  if (strcmp(start, "reboot") == 0 || strcmp(start, "restart") == 0) {
    client.println(F("Rebooting..."));
    client.stop();
    Serial.println("[DEBUG] Reboot requested via debug console");
    delay(100);  // give Serial/TCP time to flush before reset
    ESP.restart();
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

// How long STA can stay disconnected before we give up on auto-reconnect and
// bring the captive portal back.  Short enough that a non-technical user
// isn't stuck with a dead device, long enough that routine router blips
// (power outage, channel change) don't thrash the network state.
constexpr uint32_t kStaReconnectGiveUpMs = 120000;  // 2 minutes
uint32_t staDisconnectedSinceMs = 0;

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
    staDisconnectedSinceMs = 0;
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

  // Track the first moment we noticed the drop.  If auto-reconnect hasn't
  // recovered in kStaReconnectGiveUpMs, the router is gone for real (wrong
  // password after rotation, dead router, AP out of range) — drop STA and
  // come up as the captive portal so the recipient can re-provision without
  // a power cycle or serial cable.  A live job gets a grace period.
  if (staDisconnectedSinceMs == 0) {
    staDisconnectedSinceMs = millis();
  }
  if (!jobStats.active &&
      millis() - staDisconnectedSinceMs > kStaReconnectGiveUpMs) {
    Serial.println("[WIFI] Auto-reconnect gave up; switching to setup AP");
    WiFi.disconnect(/*wifioff=*/true, /*eraseap=*/false);
    delay(100);
    if (startAccessPoint()) {
      startCaptivePortal();
      setState(DeviceState::AccessPointReady);
    } else {
      Serial.println("[WIFI] Failed to bring up fallback AP");
    }
    staDisconnectedSinceMs = 0;
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

  loadProvisionedCredentials();
  if (provisionedSsid[0] != '\0') {
    Serial.printf("[BOOT] Found saved Wi-Fi credentials for \"%s\"\n",
                  provisionedSsid);
  } else {
    Serial.println("[BOOT] No saved Wi-Fi credentials; will use compile-time "
                   "defaults if set");
  }

  const bool connected = connectToWifiStation();
  if (!connected && !startAccessPoint()) {
    Serial.println("[ERROR] Failed to bring up any network mode");
    setState(DeviceState::Error);
    return;
  }
  if (!connected) {
    // AP fallback is running — spin up the captive portal so the recipient
    // can enter home Wi-Fi credentials from a phone without touching serial.
    startCaptivePortal();
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
  xTaskCreatePinnedToCore(printBufferDrainTask, "buf_drain", 6144, nullptr, 10,
                          &printBuf.drain_task, 0);
  if (printBuf.drain_task == nullptr) {
    Serial.println("[ERROR] Buffer drain task creation failed");
    setState(DeviceState::Error);
    return;
  }

  xTaskCreatePinnedToCore(usbRecvTask, "usb_recv", 4096, nullptr, 8,
                          &printBuf.recv_task, 1);
  if (printBuf.recv_task == nullptr) {
    Serial.println("[WARN] USB receive task creation failed (bidirectional disabled)");
  }

  startPrintServer();
  startDebugServer();
  logNetworkSummary();
  Serial.println("[USB] Waiting for a USB printer on the ESP32-S3 host port");
  setState(idleStateForCurrentHardware());
}

void loop() {
  updateLed();

  // If setup() bailed out early (WiFi/USB/buffer init failed) the print-data
  // path is not safe to run: the ring-buffer mutex may be null, causing
  // xSemaphoreTake(NULL, ...) to assert.  Skip print polling but keep the
  // debug console + LED alive so users can still inspect the failure over
  // the serial monitor or LAN.
  const bool printPathReady = (printBuf.mutex != nullptr);

  if (printPathReady) {
    // processPrintStream() MUST run before pollForClients().
    // pollForClients() probes the socket via connected(), which can set the
    // WiFiClient's internal _connected flag to false on peer shutdown.  Once
    // that flag is false, available() and read() short-circuit to 0/−1 and
    // any unread TCP data is silently lost.  By reading first we capture the
    // full payload while _connected is still true.
    processPrintStream();
    pollForClients();
  }
  // Debug console stays responsive even during an active print — it's the
  // only visibility users have once a job is underway.
  pollDebugServer();
  // Captive portal is only active while in AP fallback mode — services DNS
  // hijack and the HTTP form until the user saves credentials and reboots.
  pollCaptivePortal();
  restoreReadyStateIfNeeded();
  syncIdleStateWithPrinter();
  checkWifiConnection();
  logHeartbeat();
  delay(currentState == DeviceState::Printing ? 1 : 5);
}
