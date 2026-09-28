#pragma once
#include <Arduino.h>

struct NowPlayingState {
  String track = "";
  String artist = "";
  bool isPlaying = false;
  volatile bool dirty = false;  // set true whenever something above changes
};

struct NavState {
  String instruction = "";
  String distance = "";
  uint8_t maneuver = 0;  // 0=straight, 1=left, 2=right, 3=u-turn
  bool active = false;
  volatile bool dirty = false;
};

struct LyricsState {
  String currentLine = "";
  volatile bool dirty = false;
};

extern NowPlayingState nowPlaying;
extern NavState navInfo;
extern LyricsState lyricsState;

// Time is synced from the phone over BLE instead of NTP/WiFi.
// Wall-clock time = baseEpoch + (millis() - baseMillis) / 1000, shifted by utcOffsetMinutes.
extern volatile uint32_t baseEpoch;
extern volatile uint32_t baseMillis;
extern volatile int16_t utcOffsetMinutes;

// True while a phone is connected over BLE. Set by ble_service.cpp's server callbacks.
extern volatile bool phoneConnected;

// 0-100, or -1 if the battery ADC read fails to produce a sane value.
// Set by battery.cpp (reads GPIO1, this board's confirmed BAT_ADC pin).
extern int batteryPercent;
