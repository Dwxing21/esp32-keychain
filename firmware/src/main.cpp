#include <Arduino.h>
#include "display.h"
#include "input.h"
#include "ble_service.h"
#include "battery.h"
#include "time_store.h"
#include <esp_sleep.h>

void setup() {
  pinMode(0, INPUT_PULLUP);
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    while (digitalRead(0) == LOW) {
      delay(10);
    }
  }

  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-S3 smartwatch booting...");

  loadPersistedTime();  // show the last-known time immediately, before BLE reconnects
  initDisplay();
  showBootSplash();
  initInput();
  initBattery();
  initBLE();
}

void loop() {
  updateInput();
  updateBattery();
  updateDisplay();
  delay(10);
}
