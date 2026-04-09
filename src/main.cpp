#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "http_status_utils.h"
#include "network_config.h"
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
constexpr uint16_t kStatusPort = 80;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kPrintIdleTimeoutMs = 2500;
constexpr uint32_t kChunkBufferSize = 512;
constexpr size_t kHttpRequestLineMax = 256;
constexpr uint32_t kHttpClientReadTimeoutMs = 25;

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

DeviceState currentState = DeviceState::Booting;
WiFiServer printServer(kRawPrintPort);
WiFiServer statusServer(kStatusPort);
WiFiClient activeClient;
bool startedAsAccessPoint = false;
uint32_t stateChangedAtMs = 0;
uint32_t lastHeartbeatAtMs = 0;
uint32_t lastWifiLogAtMs = 0;
uint32_t lastProgressLogAtMs = 0;
uint32_t bootedAtMs = 0;

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
  Serial.printf("[NET] Status HTTP port: %u\n", kStatusPort);
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
  Serial.println("[WIFI] Connected to station network");
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

void startStatusServer() {
  statusServer.begin();
  statusServer.setNoDelay(true);
  Serial.printf("[SERVER] HTTP status server listening on port %u\n",
                kStatusPort);
}

void beginJob(WiFiClient &client) {
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

bool deliverPrintData(const uint8_t *data, size_t length) {
  return usb_printer_bridge::send_raw(data, length);
}

void pollForClients() {
  if (!activeClient || !activeClient.connected()) {
    if (jobStats.active) {
      finishJob("client disconnected");
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

  uint8_t buffer[kChunkBufferSize];
  bool sawPayload = false;

  while (activeClient.available() > 0) {
    const int bytesRead = activeClient.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) {
      break;
    }

    if (!deliverPrintData(buffer, static_cast<size_t>(bytesRead))) {
      Serial.printf("[ERROR] Printer handoff failed: %s\n",
                    usb_printer_bridge::last_error());
      setState(DeviceState::Error);
      activeClient.stop();
      finishJob("printer handoff failed", false);
      return;
    }

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
      Serial.printf("[JOB] Progress: %u bytes in %u chunks\n",
                    static_cast<unsigned>(jobStats.bytesReceived),
                    static_cast<unsigned>(jobStats.chunksReceived));
      lastProgressLogAtMs = millis();
    }
  }

  if (jobStats.active && millis() - jobStats.lastDataAtMs > kPrintIdleTimeoutMs) {
    finishJob("idle timeout");
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
      millis() - stateChangedAtMs > 1500) {
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

  if (startedAsAccessPoint) {
    Serial.printf("[HEARTBEAT] state=%s mode=AP ip=%s clients=%u usb_device=%s "
                  "usb_ready=%s vid=%04x pid=%04x out=0x%02x fwd=%u drop=%u "
                  "fail=%u\n",
                  stateName(currentState), WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPgetStationNum(),
                  usb.device_connected ? "yes" : "no",
                  usb.printer_ready ? "yes" : "no", usb.vendor_id,
                  usb.product_id, usb.out_endpoint,
                  static_cast<unsigned>(usb.total_forwarded_bytes),
                  static_cast<unsigned>(usb.total_dropped_bytes),
                  static_cast<unsigned>(usb.failed_transfer_count));
    return;
  }

  Serial.printf("[HEARTBEAT] state=%s mode=STA ip=%s rssi=%d usb_device=%s "
                "usb_ready=%s vid=%04x pid=%04x out=0x%02x fwd=%u drop=%u "
                "fail=%u\n",
                stateName(currentState), WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), usb.device_connected ? "yes" : "no",
                usb.printer_ready ? "yes" : "no", usb.vendor_id,
                usb.product_id, usb.out_endpoint,
                static_cast<unsigned>(usb.total_forwarded_bytes),
                static_cast<unsigned>(usb.total_dropped_bytes),
                static_cast<unsigned>(usb.failed_transfer_count));
}

String htmlEscape(const String &input) {
  return String(http_status_utils::EscapeHtml(input.c_str()).c_str());
}

String jsonEscape(const String &input) {
  return String(http_status_utils::EscapeJson(input.c_str()).c_str());
}

String formatStatusJson() {
  UsbPrinterBridgeStatus usb{};
  usb_printer_bridge::get_status(&usb);

  String json;
  json.reserve(1024);
  json += F("{");
  json += F("\"state\":\"");
  json += stateName(currentState);
  json += F("\",\"mode\":\"");
  json += startedAsAccessPoint ? F("AP") : F("STA");
  json += F("\",\"ip\":\"");
  json += startedAsAccessPoint ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  json += F("\",\"hostname\":\"");
  json += PRINTER_HOSTNAME;
  json += F(".local\",\"wifi_ssid\":\"");
  json += jsonEscape(startedAsAccessPoint ? String(WIFI_AP_SSID) : WiFi.SSID());
  json += F("\",\"wifi_rssi\":");
  json += String(startedAsAccessPoint ? 0 : WiFi.RSSI());
  json += F(",\"raw_port\":");
  json += String(kRawPrintPort);
  json += F(",\"status_port\":");
  json += String(kStatusPort);
  json += F(",\"uptime_ms\":");
  json += String(millis() - bootedAtMs);
  json += F(",\"job_active\":");
  json += jobStats.active ? F("true") : F("false");
  json += F(",\"job_bytes_received\":");
  json += String(jobStats.bytesReceived);
  json += F(",\"job_chunks_received\":");
  json += String(jobStats.chunksReceived);
  json += F(",\"usb_host_running\":");
  json += usb.host_running ? F("true") : F("false");
  json += F(",\"usb_device_connected\":");
  json += usb.device_connected ? F("true") : F("false");
  json += F(",\"usb_printer_ready\":");
  json += usb.printer_ready ? F("true") : F("false");
  json += F(",\"usb_dry_run_mode\":");
  json += usb.dry_run_mode ? F("true") : F("false");
  json += F(",\"usb_vendor_id\":\"");
  json += http_status_utils::FormatHex16(usb.vendor_id).c_str();
  json += F("\",\"usb_product_id\":\"");
  json += http_status_utils::FormatHex16(usb.product_id).c_str();
  json += F("\",\"usb_interface_number\":");
  json += String(usb.interface_number);
  json += F(",\"usb_out_endpoint\":");
  json += String(usb.out_endpoint);
  json += F(",\"usb_total_forwarded_bytes\":");
  json += String(usb.total_forwarded_bytes);
  json += F(",\"usb_total_dropped_bytes\":");
  json += String(usb.total_dropped_bytes);
  json += F(",\"usb_failed_transfer_count\":");
  json += String(usb.failed_transfer_count);
  json += F(",\"usb_last_error\":\"");
  json += jsonEscape(String(usb.last_error));
  json += F("\"}");
  return json;
}

String formatStatusHtml() {
  UsbPrinterBridgeStatus usb{};
  usb_printer_bridge::get_status(&usb);

  const String ip = startedAsAccessPoint ? WiFi.softAPIP().toString()
                                         : WiFi.localIP().toString();
  const String ssid = startedAsAccessPoint ? String(WIFI_AP_SSID) : WiFi.SSID();
  const String usbError = String(usb.last_error);

  String html;
  html.reserve(4096);
  html += F(
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>ESP32 Print Bridge Status</title>"
      "<style>"
      ":root{--bg:#f5f1e8;--card:#fffdf9;--ink:#1e241f;--muted:#5f6d62;"
      "--line:#d6d0c3;--ok:#2d8f47;--warn:#c98a00;--err:#c43d2f;--info:#0d7c91;}"
      "body{margin:0;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
      "background:radial-gradient(circle at top,#fff8e7,var(--bg));color:var(--ink);}"
      ".wrap{max-width:960px;margin:0 auto;padding:24px;}"
      ".hero,.card{background:var(--card);border:1px solid var(--line);border-radius:18px;"
      "box-shadow:0 10px 30px rgba(0,0,0,.06);padding:20px;margin-bottom:18px;}"
      ".hero h1{margin:0 0 8px;font-size:28px;}.hero p{margin:0;color:var(--muted);}"
      ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;}"
      ".label{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);}"
      ".value{font-size:20px;margin-top:6px;word-break:break-word;}"
      ".pill{display:inline-block;padding:6px 10px;border-radius:999px;font-size:12px;font-weight:700;}"
      ".ok{background:rgba(45,143,71,.14);color:var(--ok);}"
      ".warn{background:rgba(201,138,0,.15);color:var(--warn);}"
      ".err{background:rgba(196,61,47,.14);color:var(--err);}"
      ".info{background:rgba(13,124,145,.14);color:var(--info);}"
      "pre{white-space:pre-wrap;word-break:break-word;background:#f3efe6;border-radius:12px;padding:14px;}"
      "a{color:var(--info);text-decoration:none;}"
      "</style></head><body><div class='wrap'>");
  html += F("<section class='hero'><h1>ESP32 RAW Print Bridge</h1><p>Remote status page for TCP 9100 printing and USB printer forwarding.</p></section>");

  html += F("<section class='grid'>");
  html += F("<div class='card'><div class='label'>State</div><div class='value'>");
  html += htmlEscape(String(stateName(currentState)));
  html += F("</div></div>");
  html += F("<div class='card'><div class='label'>Mode</div><div class='value'>");
  html += startedAsAccessPoint ? F("Access Point") : F("Wi-Fi Station");
  html += F("</div></div>");
  html += F("<div class='card'><div class='label'>IP Address</div><div class='value'>");
  html += htmlEscape(ip);
  html += F("</div></div>");
  html += F("<div class='card'><div class='label'>Uptime</div><div class='value'>");
  html += String((millis() - bootedAtMs) / 1000);
  html += F(" s</div></div>");
  html += F("</section>");

  html += F("<section class='card'><div class='label'>Endpoints</div><div class='value'>RAW <strong>");
  html += String(kRawPrintPort);
  html += F("</strong> | Status <strong>");
  html += String(kStatusPort);
  html += F("</strong> | JSON <a href='/status.json'>/status.json</a></div></section>");

  html += F("<section class='grid'>");
  html += F("<div class='card'><div class='label'>Wi-Fi / AP</div><div class='value'>");
  html += htmlEscape(ssid);
  if (!startedAsAccessPoint) {
    html += F("<div class='label'>RSSI</div><div class='value'>");
    html += String(WiFi.RSSI());
    html += F(" dBm</div>");
  }
  html += F("</div></div>");

  html += F("<div class='card'><div class='label'>USB Printer</div><div class='value'>");
  html += usb.printer_ready ? F("<span class='pill ok'>READY</span>")
                            : (usb.device_connected ? F("<span class='pill warn'>DEVICE ATTACHED</span>")
                                                    : F("<span class='pill warn'>WAITING</span>"));
  html += F("<div class='label'>VID:PID</div><div class='value'>");
  html += http_status_utils::FormatHex16(usb.vendor_id).c_str();
  html += F(":");
  html += http_status_utils::FormatHex16(usb.product_id).c_str();
  html += F("</div></div></div>");

  html += F("<div class='card'><div class='label'>Current Job</div><div class='value'>");
  html += jobStats.active ? F("<span class='pill info'>ACTIVE</span>")
                          : F("<span class='pill ok'>IDLE</span>");
  html += F("<div class='label'>Bytes / Chunks</div><div class='value'>");
  html += String(jobStats.bytesReceived);
  html += F(" / ");
  html += String(jobStats.chunksReceived);
  html += F("</div></div></div>");
  html += F("</section>");

  html += F("<section class='card'><div class='label'>USB Counters</div><pre>");
  html += F("Forwarded bytes: ");
  html += String(usb.total_forwarded_bytes);
  html += F("\nDropped bytes: ");
  html += String(usb.total_dropped_bytes);
  html += F("\nFailed transfers: ");
  html += String(usb.failed_transfer_count);
  html += F("\nInterface: ");
  html += String(usb.interface_number);
  html += F("\nOUT endpoint: ");
  html += String(usb.out_endpoint);
  html += F("\nIN endpoint: ");
  html += String(usb.in_endpoint);
  html += F("</pre></section>");

  html += F("<section class='card'><div class='label'>Last USB Status</div><pre>");
  html += htmlEscape(usbError);
  html += F("</pre></section>");

  html += F("</div></body></html>");
  return html;
}

void writeHttpResponse(WiFiClient &client, const char *statusLine,
                       const char *contentType, const String &body) {
  client.print(statusLine);
  client.print(F("\r\nContent-Type: "));
  client.print(contentType);
  client.print(F("\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "));
  client.print(body.length());
  client.print(F("\r\n\r\n"));
  client.print(body);
}

void handleStatusClient(WiFiClient &client) {
  String requestLine;
  requestLine.reserve(kHttpRequestLineMax);
  bool requestLineComplete = false;

  const uint32_t deadline = millis() + kHttpClientReadTimeoutMs;
  while (millis() < deadline && requestLine.length() < kHttpRequestLineMax) {
    while (client.available() > 0 && requestLine.length() < kHttpRequestLineMax) {
      const char c = static_cast<char>(client.read());
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        requestLineComplete = true;
        break;
      }
      requestLine += c;
    }
    if (requestLineComplete) {
      break;
    }
    delay(1);
  }

  requestLine.trim();
  if (!requestLineComplete || requestLine.length() == 0 ||
      requestLine.length() >= kHttpRequestLineMax) {
    const String body = F("Bad Request\n");
    writeHttpResponse(client, "HTTP/1.1 400 Bad Request",
                      "text/plain; charset=utf-8", body);
    return;
  }

  bool saw_blank_line = false;
  String headerWindow;
  headerWindow.reserve(4);
  const uint32_t headerDeadline = millis() + kHttpClientReadTimeoutMs;
  while (millis() < headerDeadline && client.connected()) {
    while (client.available() > 0) {
      const char c = static_cast<char>(client.read());
      if (headerWindow.length() == 4) {
        headerWindow.remove(0, 1);
      }
      headerWindow += c;
      if (headerWindow.endsWith("\r\n\r\n") || headerWindow.endsWith("\n\n")) {
        saw_blank_line = true;
        break;
      }
    }
    if (saw_blank_line) {
      break;
    }
    delay(1);
  }

  if (!saw_blank_line) {
    const String body = F("Request Timeout\n");
    writeHttpResponse(client, "HTTP/1.1 408 Request Timeout",
                      "text/plain; charset=utf-8", body);
    return;
  }

  Serial.printf("[HTTP] %s\n", requestLine.c_str());

  switch (http_status_utils::ClassifyRequestLine(requestLine.c_str())) {
    case http_status_utils::RequestRoute::kStatusJson:
    writeHttpResponse(client, "HTTP/1.1 200 OK", "application/json",
                      formatStatusJson());
      return;
    case http_status_utils::RequestRoute::kRoot:
      writeHttpResponse(client, "HTTP/1.1 200 OK", "text/html; charset=utf-8",
                        formatStatusHtml());
      return;
    case http_status_utils::RequestRoute::kNotFound: {
      const String body = F("Not Found\n");
      writeHttpResponse(client, "HTTP/1.1 404 Not Found",
                        "text/plain; charset=utf-8", body);
      return;
    }
    case http_status_utils::RequestRoute::kBadRequest:
    default: {
      const String body = F("Bad Request\n");
      writeHttpResponse(client, "HTTP/1.1 400 Bad Request",
                        "text/plain; charset=utf-8", body);
      return;
    }
  }
}

void pollStatusServer() {
  WiFiClient statusClient = statusServer.available();
  if (!statusClient) {
    return;
  }

  handleStatusClient(statusClient);
  delay(5);
  statusClient.stop();
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
  bootedAtMs = millis();

  rgbLed.begin();
  rgbLed.setBrightness(32);
  rgbLed.clear();
  rgbLed.show();
  setState(DeviceState::Booting);
  updateLed();

  Serial.println();
  Serial.println("ESP32-S3 RAW TCP Print Server");
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

  startPrintServer();
  startStatusServer();
  logNetworkSummary();
  Serial.println("[USB] Waiting for a USB printer on the ESP32-S3 host port");
  setState(idleStateForCurrentHardware());
}

void loop() {
  updateLed();

  pollForClients();
  pollStatusServer();
  processPrintStream();
  restoreReadyStateIfNeeded();
  syncIdleStateWithPrinter();
  logHeartbeat();
  delay(5);
}
