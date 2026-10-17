#include "input.h"
#include "display.h"
#include "ble_service.h"
#include <Arduino.h>
#include <OneButton.h>
#include <Wire.h>
#include <esp_sleep.h>

namespace {

constexpr uint8_t TOUCH_I2C_ADDRESS = 0x15;
constexpr uint8_t TOUCH_RESET_PIN = 13;
constexpr uint8_t TOUCH_SDA_PIN = 6;
constexpr uint8_t TOUCH_SCL_PIN = 7;
constexpr uint8_t TOUCH_REGISTER_FINGER_COUNT = 0x02;
constexpr uint32_t LONG_PRESS_MS = 800;
constexpr uint32_t DOUBLE_TAP_MS = 400;
constexpr int SWIPE_THRESHOLD = 45;
constexpr int TAP_MOVE_TOLERANCE = 32;

OneButton bootButton(0, true, true);

bool touchActive = false;
bool touchLongPressHandled = false;
uint16_t touchStartX = 0;
uint16_t touchStartY = 0;
uint16_t touchLastX = 0;
uint16_t touchLastY = 0;
uint32_t touchStartMs = 0;
uint32_t lastClockTapMs = 0;

void enterDeepSleep() {
  Serial.println("[POWER] Entering deep sleep; hold BOOT to wake");
  setDisplayBacklight(false);
  while (digitalRead(0) == LOW) {
    delay(10);
  }
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

bool readTouch(uint8_t &fingerCount, uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TOUCH_I2C_ADDRESS);
  Wire.write(TOUCH_REGISTER_FINGER_COUNT);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(TOUCH_I2C_ADDRESS, static_cast<uint8_t>(5)) != 5) {
    return false;
  }

  fingerCount = Wire.read();
  const uint8_t xHigh = Wire.read();
  const uint8_t xLow = Wire.read();
  const uint8_t yHigh = Wire.read();
  const uint8_t yLow = Wire.read();
  if (fingerCount == 0 || fingerCount > 1) {
    return true;
  }

  x = static_cast<uint16_t>(((xHigh & 0x0F) << 8) | xLow);
  y = static_cast<uint16_t>(((yHigh & 0x0F) << 8) | yLow);

  // Match the 180-degree TFT rotation configured for this watch.
  x = x > 239 ? 0 : 239 - x;
  y = y > 239 ? 0 : 239 - y;
  return true;
}

void handleTap(uint16_t x, uint16_t y) {
  if (isSplashOverlayActive()) {
    dismissSplashOverlay();
    lastClockTapMs = 0;
    return;
  }

  if (isSpotifyScreenActive()) {
    if (y >= 145 && y <= 190) {
      if (x < 95) {
        notifyPlaybackCommand(PLAYBACK_PREVIOUS);
      } else if (x <= 145) {
        notifyPlaybackCommand(PLAYBACK_TOGGLE);
      } else {
        notifyPlaybackCommand(PLAYBACK_NEXT);
      }
    }
    lastClockTapMs = 0;
    return;
  }

  if (!isMainScreenActive()) {
    lastClockTapMs = 0;
    return;
  }

  const uint32_t now = millis();
  if (lastClockTapMs != 0 && now - lastClockTapMs <= DOUBLE_TAP_MS) {
    lastClockTapMs = 0;
    showSplashUntilButtonPress();
    return;
  }
  lastClockTapMs = now;
}

void handleTouchRelease(uint16_t x, uint16_t y, uint32_t durationMs) {
  const int dx = static_cast<int>(x) - static_cast<int>(touchStartX);
  const int dy = static_cast<int>(y) - static_cast<int>(touchStartY);

  if (isSplashOverlayActive()) {
    return;
  }

  if (abs(dx) >= SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
    lastClockTapMs = 0;
    if (dx < 0) {
      cycleScreen();
    } else {
      previousScreen();
    }
    return;
  }

  if (durationMs < LONG_PRESS_MS &&
      abs(dx) <= TAP_MOVE_TOLERANCE &&
      abs(dy) <= TAP_MOVE_TOLERANCE) {
    handleTap(x, y);
  }
}

void onClick() {
  if (isSplashOverlayActive()) {
    dismissSplashOverlay();
    return;
  }
  if (isSpotifyScreenActive()) {
    notifyPlaybackCommand(PLAYBACK_TOGGLE);
  } else {
    cycleScreen();
  }
}

void onDoubleClick() {
  if (isSplashOverlayActive()) {
    dismissSplashOverlay();
    return;
  }
  if (isSpotifyScreenActive()) {
    notifyPlaybackCommand(PLAYBACK_NEXT);
  } else if (isMainScreenActive()) {
    showSplashUntilButtonPress();
  } else {
    cycleScreen();
  }
}

void onLongPress() {
  if (isSplashOverlayActive()) {
    dismissSplashOverlay();
    return;
  }
  if (!isMainScreenActive() && !isDisconnectedScreenActive()) {
    cycleScreen();
    return;
  }
  enterDeepSleep();
}

void updateTouchInput() {
  uint8_t fingerCount = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  if (!readTouch(fingerCount, x, y)) {
    return;
  }

  const uint32_t now = millis();
  if (fingerCount == 1) {
    if (!touchActive) {
      touchActive = true;
      touchLongPressHandled = false;
      touchStartX = x;
      touchStartY = y;
      touchLastX = x;
      touchLastY = y;
      touchStartMs = now;
    } else {
      touchLastX = x;
      touchLastY = y;
    }
    if (touchActive && !touchLongPressHandled &&
               now - touchStartMs >= LONG_PRESS_MS &&
               (isMainScreenActive() || isDisconnectedScreenActive()) &&
               !isSplashOverlayActive()) {
      touchLongPressHandled = true;
      enterDeepSleep();
    }
    return;
  }

  if (touchActive) {
    const uint32_t durationMs = now - touchStartMs;
    if (!touchLongPressHandled) {
      handleTouchRelease(touchLastX, touchLastY, durationMs);
    }
    touchActive = false;
    touchLongPressHandled = false;
  }
}

}  // namespace

void initInput() {
  pinMode(TOUCH_RESET_PIN, OUTPUT);
  digitalWrite(TOUCH_RESET_PIN, LOW);
  delay(10);
  digitalWrite(TOUCH_RESET_PIN, HIGH);
  delay(50);
  Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
  Wire.setClock(100000);

  bootButton.attachClick(onClick);
  bootButton.attachDoubleClick(onDoubleClick);
  bootButton.attachLongPressStart(onLongPress);
  bootButton.setPressMs(600);
  bootButton.setClickMs(350);
}

void updateInput() {
  bootButton.tick();
  updateTouchInput();
}
