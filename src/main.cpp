#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#ifndef RGB_LED_PIN
#define RGB_LED_PIN 48
#endif

#ifndef RGB_LED_COUNT
#define RGB_LED_COUNT 1
#endif

#ifndef BLINK_INTERVAL_MS
#define BLINK_INTERVAL_MS 500
#endif

Adafruit_NeoPixel rgbLed(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

static void setLedColor(uint32_t color) {
  rgbLed.setPixelColor(0, color);
  rgbLed.show();
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("Hello from ESP32-S3 N16R8");
  Serial.printf("CPU frequency: %lu MHz\n", getCpuFrequencyMhz());

  if (psramInit()) {
    Serial.printf("PSRAM detected: %u bytes\n", ESP.getPsramSize());
  } else {
    Serial.println("PSRAM not detected");
  }

  rgbLed.begin();
  rgbLed.setBrightness(32);
  rgbLed.clear();
  rgbLed.show();

  Serial.printf("Blinking RGB LED on GPIO %d\n", RGB_LED_PIN);
}

void loop() {
  static bool ledOn = false;
  ledOn = !ledOn;

  if (ledOn) {
    setLedColor(rgbLed.Color(0, 48, 0));
    Serial.println("Blink ON");
  } else {
    setLedColor(rgbLed.Color(0, 0, 0));
    Serial.println("Blink OFF");
  }

  delay(BLINK_INTERVAL_MS);
}
