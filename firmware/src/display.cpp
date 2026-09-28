#include "display.h"
#include "shared_state.h"
#include "icons.h"
#include <TFT_eSPI.h>
#include <time.h>

TFT_eSPI tft = TFT_eSPI();

static uint8_t currentScreen = 0;  // 0 = main/clock, 1 = Spotify, 2 = lyrics, 3 = navigation
static uint32_t lastClockRedraw = 0;
static uint32_t lastDisconnectedRedraw = 0;
static bool forceRedraw = true;

// Boot behavior: start on the main screen, but if the phone reports an active
// nav route or playing music before the user touches the button, jump there
// once. After that one jump (or after any manual button press), this turns
// off for the rest of the session -- no more auto-switching.
static bool autoFollowActive = true;

// Picks the closest of the 5 battery bitmaps (100/75/50/25/0%) for a given percent.
static int batteryBucket(int percent) {
  if (percent >= 88) return 100;
  if (percent >= 63) return 75;
  if (percent >= 38) return 50;
  if (percent >= 13) return 25;
  return 0;
}

// Draws the small (26x13) or large (50x24) battery icon centered at (cx, cy).
// If batteryPercent is unknown (-1, no ADC pin wired up yet -- see battery.cpp),
// draws the empty-outline icon with "--" next to it instead of a fill level.
static void drawBatteryIcon(int cx, int cy, bool large) {
  const unsigned char* bitmap;
  int w, h;
  if (batteryPercent >= 0) {
    int bucket = batteryBucket(batteryPercent);
    if (large) {
      switch (bucket) {
        case 100: bitmap = icon_batt_lg_100; break;
        case 75:  bitmap = icon_batt_lg_75;  break;
        case 50:  bitmap = icon_batt_lg_50;  break;
        case 25:  bitmap = icon_batt_lg_25;  break;
        default:  bitmap = icon_batt_lg_0;   break;
      }
      w = ICON_BATT_LG_100_W; h = ICON_BATT_LG_100_H;
    } else {
      switch (bucket) {
        case 100: bitmap = icon_batt_sm_100; break;
        case 75:  bitmap = icon_batt_sm_75;  break;
        case 50:  bitmap = icon_batt_sm_50;  break;
        case 25:  bitmap = icon_batt_sm_25;  break;
        default:  bitmap = icon_batt_sm_0;   break;
      }
      w = ICON_BATT_SM_100_W; h = ICON_BATT_SM_100_H;
    }
  } else {
    bitmap = large ? icon_batt_lg_0 : icon_batt_sm_0;
    w = large ? ICON_BATT_LG_100_W : ICON_BATT_SM_100_W;
    h = large ? ICON_BATT_LG_100_H : ICON_BATT_SM_100_H;
  }

  uint16_t color = batteryPercent >= 0 && batteryPercent <= 20 ? TFT_RED : TFT_WHITE;
  tft.drawXBitmap(cx - w / 2, cy - h / 2, bitmap, w, h, color);

  if (batteryPercent < 0) {
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("--", cx + w / 2 + 4, cy, 1);
  }
}

static void drawDisconnectedScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawXBitmap(120 - ICON_BT_DISCONNECTED_24_W / 2, 55, icon_bt_disconnected_24,
                   ICON_BT_DISCONNECTED_24_W, ICON_BT_DISCONNECTED_24_H, TFT_DARKGREY);
  drawBatteryIcon(120, 110, /*large=*/true);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connect to phone", 120, 160, 2);
}

static void drawClockScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (baseEpoch == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("waiting for phone", 120, 120, 2);
    drawBatteryIcon(172, 36, /*large=*/false);
    return;
  }

  uint32_t nowEpoch = baseEpoch + (millis() - baseMillis) / 1000;
  // Shifting the epoch by the UTC offset and reading it back with gmtime()
  // is a simple way to get "local" wall-clock fields without a timezone database.
  int32_t localEpoch = (int32_t)nowEpoch + (int32_t)utcOffsetMinutes * 60;
  time_t t = (time_t)localEpoch;
  struct tm* lt = gmtime(&t);

  char timeStr[9];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%02d-%02d-%04d", lt->tm_mday, lt->tm_mon + 1, lt->tm_year + 1900);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(timeStr, 120, 110, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(dateStr, 120, 150, 2);

  drawBatteryIcon(172, 36, /*large=*/false);
}

// Playback button hit-boxes, drawn every time this screen redraws.
// (kept here so drawSpotifyScreen and a future touch/button handler agree on where they are)
struct PlaybackButton {
  int cx, cy, r;
};
static const PlaybackButton kPrevButton = {70, 190, 22};
static const PlaybackButton kPlayButton = {120, 190, 26};
static const PlaybackButton kNextButton = {170, 190, 22};

static void drawPlaybackButton(const PlaybackButton& b, bool filled) {
  tft.fillCircle(b.cx, b.cy, b.r, filled ? TFT_DARKGREEN : TFT_DARKGREY);
  tft.drawCircle(b.cx, b.cy, b.r, TFT_WHITE);
}

// Truncates text with "..." if it's wider than maxWidth at the given font,
// so long track/artist/instruction names can't run off the round display.
static String truncateToFit(const String& text, uint8_t font, int maxWidth) {
  if (tft.textWidth(text, font) <= maxWidth) return text;
  String result = text;
  while (result.length() > 0 && tft.textWidth(result + "...", font) > maxWidth) {
    result.remove(result.length() - 1);
  }
  return result + "...";
}

// Greedy word-wraps text into up to maxLines lines (each <= maxWidth at the given
// font), centered as a block around (cx, cyCenter). Adds "..." to the last line
// if there's more text than fits. Used for lyrics, which run longer than a
// single title/instruction line.
static void drawWrappedText(const String& text, int cx, int cyCenter, uint8_t font,
                             int maxWidth, int maxLines, uint16_t color) {
  static const int kLineHeight = 20;
  String lines[8];
  int lineCount = 0;
  String current = "";
  String remaining = text;

  while (remaining.length() > 0 && lineCount < maxLines) {
    int spaceIdx = remaining.indexOf(' ');
    String word = spaceIdx == -1 ? remaining : remaining.substring(0, spaceIdx);
    String candidate = current.length() == 0 ? word : current + " " + word;
    if (tft.textWidth(candidate, font) <= maxWidth) {
      current = candidate;
      remaining = spaceIdx == -1 ? "" : remaining.substring(spaceIdx + 1);
    } else {
      if (current.length() == 0) {
        // a single word wider than maxWidth on its own -- take it anyway
        current = word;
        remaining = spaceIdx == -1 ? "" : remaining.substring(spaceIdx + 1);
      }
      lines[lineCount++] = current;
      current = "";
    }
  }
  if (current.length() > 0 && lineCount < maxLines) {
    lines[lineCount++] = current;
  }
  if (remaining.length() > 0 && lineCount == maxLines && lineCount > 0) {
    String last = lines[lineCount - 1];
    while (last.length() > 0 && tft.textWidth(last + "...", font) > maxWidth) {
      last.remove(last.length() - 1);
    }
    lines[lineCount - 1] = last + "...";
  }

  int totalHeight = kLineHeight * lineCount;
  int yStart = cyCenter - totalHeight / 2 + kLineHeight / 2;
  tft.setTextColor(color, TFT_BLACK);
  for (int i = 0; i < lineCount; i++) {
    tft.drawString(lines[i], cx, yStart + i * kLineHeight, font);
  }
}

static void drawSpotifyScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (nowPlaying.track.length() == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("no track playing", 120, 120, 2);
    return;
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(truncateToFit(nowPlaying.track, 2, 200), 120, 70, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(truncateToFit(nowPlaying.artist, 2, 200), 120, 95, 2);

  // Playback buttons
  drawPlaybackButton(kPrevButton, false);
  tft.drawXBitmap(kPrevButton.cx - ICON_PREV_28_W / 2, kPrevButton.cy - ICON_PREV_28_H / 2,
                   icon_prev_28, ICON_PREV_28_W, ICON_PREV_28_H, TFT_WHITE);

  drawPlaybackButton(kPlayButton, nowPlaying.isPlaying);
  if (nowPlaying.isPlaying) {
    tft.drawXBitmap(kPlayButton.cx - ICON_PAUSE_32_W / 2, kPlayButton.cy - ICON_PAUSE_32_H / 2,
                     icon_pause_32, ICON_PAUSE_32_W, ICON_PAUSE_32_H, TFT_WHITE);
  } else {
    tft.drawXBitmap(kPlayButton.cx - ICON_PLAY_32_W / 2, kPlayButton.cy - ICON_PLAY_32_H / 2,
                     icon_play_32, ICON_PLAY_32_W, ICON_PLAY_32_H, TFT_WHITE);
  }

  drawPlaybackButton(kNextButton, false);
  tft.drawXBitmap(kNextButton.cx - ICON_NEXT_28_W / 2, kNextButton.cy - ICON_NEXT_28_H / 2,
                   icon_next_28, ICON_NEXT_28_W, ICON_NEXT_28_H, TFT_WHITE);
}

static void drawLyricsScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (lyricsState.currentLine.length() == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("no lyrics", 120, 120, 2);
    return;
  }

  drawWrappedText(lyricsState.currentLine, 120, 120, 2, 200, 3, TFT_WHITE);
}

static const unsigned char* maneuverIcon(uint8_t code, int* w, int* h) {
  *w = ICON_ARROW_STRAIGHT_40_W;
  *h = ICON_ARROW_STRAIGHT_40_H;
  switch (code) {
    case 1: return icon_arrow_left_40;
    case 2: return icon_arrow_right_40;
    case 3: return icon_arrow_uturn_40;
    default: return icon_arrow_straight_40;
  }
}

static void drawNavScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (!navInfo.active) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("no active route", 120, 120, 2);
    return;
  }

  int w, h;
  const unsigned char* icon = maneuverIcon(navInfo.maneuver, &w, &h);
  tft.drawXBitmap(120 - w / 2, 45, icon, w, h, TFT_CYAN);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(truncateToFit(navInfo.instruction, 2, 200), 120, 135, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(navInfo.distance, 120, 170, 4);
}

static void drawCurrentScreen() {
  // The Spotify and nav screens are meaningless without the phone, so show
  // the "connect to phone" screen instead when disconnected. The main/clock
  // screen keeps working on its own (best-known/persisted time), so it's
  // exempt -- see notifyPhoneConnected()/notifyPhoneDisconnected() below.
  if (!phoneConnected && currentScreen != 0) {
    drawDisconnectedScreen();
    return;
  }
  switch (currentScreen) {
    case 0: drawClockScreen(); break;
    case 1: drawSpotifyScreen(); break;
    case 2: drawLyricsScreen(); break;
    case 3: drawNavScreen(); break;
  }
}

void initDisplay() {
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  // Note: GPIO0 (BOOT) is now owned by input.cpp's OneButton instance, not read here.
}

void cycleScreen() {
  currentScreen = (currentScreen + 1) % 4;
  forceRedraw = true;
  autoFollowActive = false;  // user took manual control -- stop auto-jumping for this session
}

bool isSpotifyScreenActive() {
  return currentScreen == 1;
}

void notifyPhoneConnected() {
  // Treat each new connection like a fresh boot for screen selection: start
  // at main and let the auto-follow logic below decide if something's
  // already active.
  currentScreen = 0;
  autoFollowActive = true;
  forceRedraw = true;
}

void notifyPhoneDisconnected() {
  forceRedraw = true;  // switch to the "connect to phone" screen immediately, if applicable
}

void updateDisplay() {
  // One-time boot behavior: jump off the main screen if something's already
  // active by the time the phone reports in, but only until the user presses
  // the button themselves (see cycleScreen()) or this has already fired once.
  if (autoFollowActive && currentScreen == 0) {
    if (navInfo.active) {
      currentScreen = 3;
      forceRedraw = true;
      autoFollowActive = false;
    } else if (nowPlaying.isPlaying) {
      currentScreen = 1;
      forceRedraw = true;
      autoFollowActive = false;
    }
  }

  bool needRedraw = forceRedraw;
  bool showingDisconnected = (!phoneConnected && currentScreen != 0);

  if (showingDisconnected) {
    if (millis() - lastDisconnectedRedraw > 2000) {  // refresh the battery level periodically
      needRedraw = true;
      lastDisconnectedRedraw = millis();
    }
  } else if (currentScreen == 0 && millis() - lastClockRedraw > 1000) {
    needRedraw = true;
    lastClockRedraw = millis();
  } else if (currentScreen == 1 && nowPlaying.dirty) {
    needRedraw = true;
    nowPlaying.dirty = false;
  } else if (currentScreen == 2 && lyricsState.dirty) {
    needRedraw = true;
    lyricsState.dirty = false;
  } else if (currentScreen == 3 && navInfo.dirty) {
    needRedraw = true;
    navInfo.dirty = false;
  }

  if (needRedraw) {
    drawCurrentScreen();
    forceRedraw = false;
  }
}
