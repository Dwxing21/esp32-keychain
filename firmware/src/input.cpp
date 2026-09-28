#include "input.h"
#include "display.h"
#include "ble_service.h"
#include <OneButton.h>

// GPIO0 is the BOOT button on this board: active low, needs the internal pull-up.
static OneButton bootButton(0, true, true);

static void onClick() {
  if (isSpotifyScreenActive()) {
    notifyPlaybackCommand(PLAYBACK_TOGGLE);
  } else {
    cycleScreen();
  }
}

static void onDoubleClick() {
  if (isSpotifyScreenActive()) {
    notifyPlaybackCommand(PLAYBACK_NEXT);
  } else {
    cycleScreen();
  }
}

static void onLongPress() {
  // Always cycles screens -- this is the reliable way off the Spotify screen
  // regardless of what click/double-click mean there.
  cycleScreen();
}

void initInput() {
  bootButton.attachClick(onClick);
  bootButton.attachDoubleClick(onDoubleClick);
  bootButton.attachLongPressStart(onLongPress);
  bootButton.setPressMs(600);  // hold >600ms counts as a long press
  bootButton.setClickMs(350);  // window to detect a second click as "double"
}

void updateInput() {
  bootButton.tick();
}
