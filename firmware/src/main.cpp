#include <Arduino.h>
#include "display.h"
#include "input.h"
#include "ble_service.h"
#include "battery.h"
#include "time_store.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-S3 smartwatch booting...");

  loadPersistedTime();  // show the last-known time immediately, before BLE reconnects
  initDisplay();
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
