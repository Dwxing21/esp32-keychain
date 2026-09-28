#include "ble_service.h"
#include "shared_state.h"
#include "display.h"
#include "time_store.h"
#include <NimBLEDevice.h>

// ---- Service / characteristic UUIDs ----
// These must match BleUuids in the Android app's BleGattClient.kt exactly.
#define NOWPLAYING_SERVICE_UUID   "c9a01000-1000-4bd9-ba21-70e224d9b8a0"
#define CHAR_TRACK_INFO_UUID      "c9a01001-1000-4bd9-ba21-70e224d9b8a0"
#define CHAR_PLAYBACK_STATE_UUID  "c9a01004-1000-4bd9-ba21-70e224d9b8a0"
#define CHAR_PLAYBACK_CMD_UUID    "c9a01005-1000-4bd9-ba21-70e224d9b8a0"  // watch -> phone, NOTIFY
#define CHAR_LYRICS_LINE_UUID     "c9a01006-1000-4bd9-ba21-70e224d9b8a0"

#define NAV_SERVICE_UUID          "c9a02000-1000-4bd9-ba21-70e224d9b8a0"
#define CHAR_NAV_INSTRUCTION_UUID "c9a02001-1000-4bd9-ba21-70e224d9b8a0"
#define CHAR_NAV_ACTIVE_UUID      "c9a02002-1000-4bd9-ba21-70e224d9b8a0"

#define TIME_SERVICE_UUID         "c9a03000-1000-4bd9-ba21-70e224d9b8a0"
#define CHAR_SET_TIME_UUID        "c9a03001-1000-4bd9-ba21-70e224d9b8a0"

// Global state definitions (declared extern in shared_state.h)
NowPlayingState nowPlaying;
NavState navInfo;
LyricsState lyricsState;
volatile uint32_t baseEpoch = 0;
volatile uint32_t baseMillis = 0;
volatile int16_t utcOffsetMinutes = 0;
volatile bool phoneConnected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server) override {
    Serial.println("[BLE] Phone connected");
    phoneConnected = true;
    notifyPhoneConnected();  // re-arms boot-style auto-follow, forces a redraw
  }
  void onDisconnect(NimBLEServer* server) override {
    Serial.println("[BLE] Phone disconnected, restarting advertising");
    phoneConnected = false;
    notifyPhoneDisconnected();  // forces an immediate redraw to the "no phone" state
    NimBLEDevice::startAdvertising();
  }
};

// Payload format: "<track>\n<artist>\n<0 or 1>"
class TrackInfoCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    size_t firstNl = v.find('\n');
    size_t secondNl = v.find('\n', firstNl + 1);
    if (firstNl == std::string::npos || secondNl == std::string::npos) return;
    nowPlaying.track = String(v.substr(0, firstNl).c_str());
    nowPlaying.artist = String(v.substr(firstNl + 1, secondNl - firstNl - 1).c_str());
    nowPlaying.isPlaying = v.substr(secondNl + 1) == "1";
    nowPlaying.dirty = true;
  }
};

class PlaybackStateCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    nowPlaying.isPlaying = (v[0] == 1);
    nowPlaying.dirty = true;
  }
};

// Payload format: "<instruction>\n<distance>\n<maneuverCode>"
class NavInstructionCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    size_t firstNl = v.find('\n');
    size_t secondNl = v.find('\n', firstNl + 1);
    if (firstNl == std::string::npos || secondNl == std::string::npos) return;
    navInfo.instruction = String(v.substr(0, firstNl).c_str());
    navInfo.distance = String(v.substr(firstNl + 1, secondNl - firstNl - 1).c_str());
    navInfo.maneuver = (uint8_t)atoi(v.substr(secondNl + 1).c_str());
    navInfo.active = true;
    navInfo.dirty = true;
  }
};

class NavActiveCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    navInfo.active = (v[0] == 1);
    navInfo.dirty = true;
  }
};

// Plain text: the current lyric line, sent whenever it changes. Empty string
// means "no lyrics" (either none found for this track, or nothing loaded yet).
class LyricsLineCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    lyricsState.currentLine = String(v.c_str());
    lyricsState.dirty = true;
  }
};

// 6 bytes little-endian: uint32 epoch seconds + int16 UTC offset in minutes
class SetTimeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.size() < 6) return;
    uint32_t epoch;
    int16_t offset;
    memcpy(&epoch, v.data(), 4);
    memcpy(&offset, v.data() + 4, 2);
    baseEpoch = epoch;
    baseMillis = millis();
    utcOffsetMinutes = offset;
    persistTime();  // save it so a reboot starts from here instead of blank
    Serial.printf("[BLE] Time synced: epoch=%u offset=%d min\n", epoch, offset);
  }
};

static NimBLECharacteristic* g_playbackCmdChar = nullptr;

void notifyPlaybackCommand(PlaybackCommand cmd) {
  if (g_playbackCmdChar == nullptr) return;
  uint8_t value = (uint8_t)cmd;
  g_playbackCmdChar->setValue(&value, 1);
  g_playbackCmdChar->notify();
}

void initBLE() {
  NimBLEDevice::init("ESP32 Smartwatch");
  NimBLEDevice::setMTU(247);  // request a larger MTU so text/art chunks move faster

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  // --- Now Playing service ---
  NimBLEService* nowPlayingService = pServer->createService(NOWPLAYING_SERVICE_UUID);

  NimBLECharacteristic* trackInfoChar =
      nowPlayingService->createCharacteristic(CHAR_TRACK_INFO_UUID, NIMBLE_PROPERTY::WRITE);
  trackInfoChar->setCallbacks(new TrackInfoCallbacks());

  NimBLECharacteristic* playbackStateChar =
      nowPlayingService->createCharacteristic(CHAR_PLAYBACK_STATE_UUID, NIMBLE_PROPERTY::WRITE);
  playbackStateChar->setCallbacks(new PlaybackStateCallbacks());

  g_playbackCmdChar =
      nowPlayingService->createCharacteristic(CHAR_PLAYBACK_CMD_UUID, NIMBLE_PROPERTY::NOTIFY);

  NimBLECharacteristic* lyricsLineChar =
      nowPlayingService->createCharacteristic(CHAR_LYRICS_LINE_UUID, NIMBLE_PROPERTY::WRITE);
  lyricsLineChar->setCallbacks(new LyricsLineCallbacks());

  nowPlayingService->start();

  // --- Navigation service ---
  NimBLEService* navService = pServer->createService(NAV_SERVICE_UUID);

  NimBLECharacteristic* navInstructionChar =
      navService->createCharacteristic(CHAR_NAV_INSTRUCTION_UUID, NIMBLE_PROPERTY::WRITE);
  navInstructionChar->setCallbacks(new NavInstructionCallbacks());

  NimBLECharacteristic* navActiveChar =
      navService->createCharacteristic(CHAR_NAV_ACTIVE_UUID, NIMBLE_PROPERTY::WRITE);
  navActiveChar->setCallbacks(new NavActiveCallbacks());

  navService->start();

  // --- Time service ---
  NimBLEService* timeService = pServer->createService(TIME_SERVICE_UUID);
  NimBLECharacteristic* setTimeChar =
      timeService->createCharacteristic(CHAR_SET_TIME_UUID, NIMBLE_PROPERTY::WRITE);
  setTimeChar->setCallbacks(new SetTimeCallbacks());
  timeService->start();

  // --- Advertising ---
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(NOWPLAYING_SERVICE_UUID);
  advertising->addServiceUUID(NAV_SERVICE_UUID);
  advertising->addServiceUUID(TIME_SERVICE_UUID);
  advertising->setName("ESP32 Smartwatch");
  advertising->start();

  Serial.println("[BLE] GATT server up, advertising as 'ESP32 Smartwatch'");
}
