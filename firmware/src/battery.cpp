#include "battery.h"
#include "shared_state.h"
#include <Arduino.h>

// Confirmed for this board family (ESP32-S3-LCD-1.28): GPIO1 is
// BAT_ADC, wired through an onboard divider for battery voltage sensing.
// (If you're using a different/newer board revision, double-check against
// its schematic -- but this matches the documented pinout for this model.)
#define BATTERY_ADC_PIN 1

#ifndef BATTERY_DIVIDER_RATIO
#define BATTERY_DIVIDER_RATIO 3.0f
#endif

int batteryPercent = -1;
uint16_t batteryVoltageMv = 0;

static uint32_t lastCheck = 0;
static bool hasSample = false;
static uint16_t filteredBatteryMv = 0;

struct VoltagePercentPoint {
  uint16_t millivolts;
  uint8_t percent;
};

// Approximate single-cell LiPo open-circuit discharge curve. It is an estimate,
// not a fuel gauge; battery chemistry, load, age, and temperature affect it.
static const VoltagePercentPoint dischargeCurve[] = {
    {3300, 0}, {3500, 5}, {3600, 10}, {3650, 20}, {3700, 30},
    {3750, 40}, {3800, 50}, {3850, 60}, {3900, 70}, {3950, 80},
    {4050, 90}, {4150, 95}, {4200, 100},
};

static uint8_t estimatePercent(uint16_t voltageMv) {
  const size_t pointCount = sizeof(dischargeCurve) / sizeof(dischargeCurve[0]);
  if (voltageMv <= dischargeCurve[0].millivolts) return 0;
  if (voltageMv >= dischargeCurve[pointCount - 1].millivolts) return 100;

  for (size_t i = 1; i < pointCount; ++i) {
    if (voltageMv <= dischargeCurve[i].millivolts) {
      const VoltagePercentPoint& lower = dischargeCurve[i - 1];
      const VoltagePercentPoint& upper = dischargeCurve[i];
      const uint32_t voltageRange = upper.millivolts - lower.millivolts;
      const uint32_t voltageOffset = voltageMv - lower.millivolts;
      const uint32_t percentRange = upper.percent - lower.percent;
      return lower.percent + (voltageOffset * percentRange + voltageRange / 2) / voltageRange;
    }
  }
  return 100;
}

void initBattery() {
#if BATTERY_ADC_PIN >= 0
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);  // full 0-3.3V range
#endif
}

void updateBattery() {
#if BATTERY_ADC_PIN >= 0
  if (hasSample && millis() - lastCheck < 5000) return;
  lastCheck = millis();
  hasSample = true;

  static const uint8_t kSampleCount = 9;
  uint16_t samples[kSampleCount];
  uint16_t rawSamples[kSampleCount];
  for (uint8_t i = 0; i < kSampleCount; ++i) {
    samples[i] = (uint16_t)analogReadMilliVolts(BATTERY_ADC_PIN);
    rawSamples[i] = (uint16_t)analogRead(BATTERY_ADC_PIN);
  }

  for (uint8_t i = 1; i < kSampleCount; ++i) {
    const uint16_t value = samples[i];
    const uint16_t rawValue = rawSamples[i];
    int8_t j = i - 1;
    while (j >= 0 && samples[j] > value) {
      samples[j + 1] = samples[j];
      rawSamples[j + 1] = rawSamples[j];
      --j;
    }
    samples[j + 1] = value;
    rawSamples[j + 1] = rawValue;
  }

  const uint16_t adcMedianMv = samples[kSampleCount / 2];
  const uint16_t adcMedianRaw = rawSamples[kSampleCount / 2];
  if (adcMedianMv == 0 || adcMedianMv >= 3300) {
    batteryPercent = -1;
    batteryVoltageMv = 0;
    Serial.printf("[Battery] ADC out of range: raw=%u, pin=%u mV\n",
                  adcMedianRaw, adcMedianMv);
    return;
  }

  const uint16_t measuredBatteryMv =
      (uint16_t)((uint32_t)adcMedianMv * BATTERY_DIVIDER_RATIO + 0.5f);
  if (filteredBatteryMv == 0) {
    filteredBatteryMv = measuredBatteryMv;
  } else {
    // Smooth load-induced/ADC jitter while still following real battery changes.
    filteredBatteryMv = (uint16_t)(((uint32_t)filteredBatteryMv * 3 + measuredBatteryMv + 2) / 4);
  }

  batteryVoltageMv = filteredBatteryMv;
  batteryPercent = estimatePercent(batteryVoltageMv);
  Serial.printf("[Battery] raw=%u, pin=%u mV, battery=%u mV (ratio %.2f), estimated %d%%\n",
                adcMedianRaw, adcMedianMv, batteryVoltageMv,
                (float)BATTERY_DIVIDER_RATIO, batteryPercent);
#else
  batteryPercent = -1;
  batteryVoltageMv = 0;
#endif
}
