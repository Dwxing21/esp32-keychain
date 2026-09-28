#include "battery.h"
#include "shared_state.h"
#include <Arduino.h>

// Confirmed for this exact board (ESP32-S3-LCD-1.28, non-touch): GPIO1 is
// BAT_ADC, wired through an onboard divider for battery voltage sensing.
// (If you're using a different/newer board revision, double-check against
// its schematic -- but this matches the documented pinout for this model.)
#define BATTERY_ADC_PIN 1

// If BATTERY_ADC_PIN is set: the ratio between the actual battery voltage and
// what's measured at the ADC pin. A common resistor divider halves the
// voltage, in which case this should be 2.0. Adjust to match your board.
#define DIVIDER_RATIO 2.0f

// LiPo voltage range this maps to 0-100%. These are reasonable general
// defaults, not tuned to a specific cell.
#define BATTERY_EMPTY_V 3.3f
#define BATTERY_FULL_V  4.2f

int batteryPercent = -1;

static uint32_t lastCheck = 0;

void initBattery() {
#if BATTERY_ADC_PIN >= 0
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);  // full 0-3.3V range
#endif
}

void updateBattery() {
#if BATTERY_ADC_PIN >= 0
  if (millis() - lastCheck < 5000) return;  // check every 5s, no need for more
  lastCheck = millis();

  int raw = analogRead(BATTERY_ADC_PIN);
  float pinVoltage = (raw / 4095.0f) * 3.3f;
  float batteryVoltage = pinVoltage * DIVIDER_RATIO;

  float percent = (batteryVoltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0f;
  batteryPercent = (int)constrain(percent, 0.0f, 100.0f);
#else
  batteryPercent = -1;
#endif
}
