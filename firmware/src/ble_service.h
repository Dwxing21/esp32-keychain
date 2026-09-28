#pragma once

// Sets up the BLE GATT server: advertises as "ESP32 Smartwatch" and exposes
// three services the Android companion app writes into:
//   - Now Playing service (track/artist/playback state, album art)
//   - Navigation service (turn instruction/distance)
//   - Time service (phone pushes epoch + UTC offset so the watch needs no WiFi/RTC)
void initBLE();

// Playback commands the watch can send to the phone (BOOT button on the Spotify screen).
enum PlaybackCommand : uint8_t {
  PLAYBACK_TOGGLE = 0,
  PLAYBACK_NEXT = 1,
  PLAYBACK_PREVIOUS = 2,  // not wired to a gesture yet -- reserved for a second button
};

// Notifies the connected phone's subscribed characteristic. No-op if nothing is connected/subscribed.
void notifyPlaybackCommand(PlaybackCommand cmd);
