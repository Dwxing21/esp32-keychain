#include "time_store.h"
#include "shared_state.h"
#include <Preferences.h>

static Preferences prefs;
static const char* kNamespace = "watch-time";

void loadPersistedTime() {
  prefs.begin(kNamespace, /*readOnly=*/true);
  uint32_t savedEpoch = prefs.getUInt("epoch", 0);
  int16_t savedOffset = prefs.getShort("offset", 0);
  prefs.end();

  if (savedEpoch > 0) {
    baseEpoch = savedEpoch;
    baseMillis = millis();
    utcOffsetMinutes = savedOffset;
    Serial.printf("[TIME] Restored last-known time from flash (epoch=%u)\n", savedEpoch);
  } else {
    Serial.println("[TIME] No persisted time found -- will show 'waiting for phone' until first sync");
  }
}

void persistTime() {
  prefs.begin(kNamespace, /*readOnly=*/false);
  prefs.putUInt("epoch", baseEpoch);
  prefs.putShort("offset", utcOffsetMinutes);
  prefs.end();
}
