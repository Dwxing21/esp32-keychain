#include "display.h"
#include "shared_state.h"
#include "ble_service.h"
#include "icons.h"
#include "splash_image.h"
#include <TFT_eSPI.h>
#include <time.h>

TFT_eSPI tft = TFT_eSPI();

static uint8_t currentScreen = 0;  // 0 = main/clock, 1 = Spotify, 2 = lyrics, 3 = navigation
static uint32_t lastClockSecond = UINT32_MAX;
static uint32_t lastClockDay = UINT32_MAX;
static uint32_t lastSpotifySecond = UINT32_MAX;
static int lastDrawnBatteryPercent = -2;
static bool forceRedraw = true;
static bool splashOverlayActive = false;

// Boot behavior: start on the main screen, but if the phone reports an active
// nav route or playing music before the user touches the button, jump there
// once. After that one jump (or after any manual button press), this turns
// off for the rest of the session -- no more auto-switching.
static bool autoFollowActive = true;

// Draw a crisp battery outline and proportional fill at either display size.
static void drawBatteryIcon(int cx, int cy, bool large) {
  const int w = large ? 50 : 26;
  const int h = large ? 24 : 13;
  const int x = cx - w / 2;
  const int y = cy - h / 2;
  const int bodyWidth = w - 4;
  uint16_t color = batteryPercent >= 0 && batteryPercent <= 20 ? TFT_RED : TFT_WHITE;
  tft.drawRect(x, y, bodyWidth, h, color);
  tft.fillRect(x + bodyWidth, y + h / 3, 3, max(1, h / 3), color);

  if (batteryPercent > 0) {
    const int innerWidth = bodyWidth - 4;
    const int fillWidth = innerWidth * constrain(batteryPercent, 0, 100) / 100;
    if (fillWidth > 0) {
      tft.fillRect(x + 2, y + 2, fillWidth, h - 4, color);
    }
  }

  if (batteryPercent >= 0) {
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(String(batteryPercent) + "%", x - 4, cy, 1);
  } else {
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("--", x - 4, cy, 1);
  }
}

static void drawDisconnectedScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  const uint16_t iconColor = TFT_DARKGREY;
  tft.drawLine(120, 54, 120, 82, iconColor);
  tft.drawLine(120, 54, 130, 62, iconColor);
  tft.drawLine(130, 62, 120, 69, iconColor);
  tft.drawLine(120, 69, 130, 77, iconColor);
  tft.drawLine(130, 77, 120, 82, iconColor);
  tft.drawLine(112, 57, 128, 80, iconColor);
  drawBatteryIcon(120, 110, /*large=*/true);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connect to phone", 120, 160, 2);
}

static uint32_t currentLocalEpoch() {
  uint32_t nowEpoch = baseEpoch + (millis() - baseMillis) / 1000;
  return (uint32_t)((int32_t)nowEpoch + (int32_t)utcOffsetMinutes * 60);
}

static void drawClockTime() {
  if (baseEpoch == 0) return;
  uint32_t localEpoch = currentLocalEpoch();
  time_t t = (time_t)localEpoch;
  struct tm* lt = gmtime(&t);
  char timeStr[9];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
  tft.fillRect(20, 85, 200, 50, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(timeStr, 120, 110, 4);
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

  uint32_t localEpoch = currentLocalEpoch();
  time_t t = (time_t)localEpoch;
  struct tm* lt = gmtime(&t);
  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%02d-%02d-%04d", lt->tm_mday, lt->tm_mon + 1, lt->tm_year + 1900);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(dateStr, 120, 150, 2);
  drawBatteryIcon(172, 36, /*large=*/false);
  drawClockTime();
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

// Greedy word-wraps text into up to maxLines lines, centered around (cx, cyCenter).
static bool drawWrappedText(const String& text, int cx, int cyCenter, uint8_t font,
                             int maxWidth, int maxLines, int lineHeight, uint16_t color) {
  String lines[8];
  int lineCount = 0;
  String current = "";
  String remaining = text;
  bool truncated = false;

  while (remaining.length() > 0 && lineCount < maxLines) {
    while (remaining.startsWith(" ")) remaining.remove(0, 1);
    if (remaining.length() == 0) break;

    int spaceIdx = remaining.indexOf(' ');
    String word = spaceIdx == -1 ? remaining : remaining.substring(0, spaceIdx);
    remaining = spaceIdx == -1 ? "" : remaining.substring(spaceIdx + 1);

    String candidate = current.length() == 0 ? word : current + " " + word;
    if (tft.textWidth(candidate, font) <= maxWidth) {
      current = candidate;
      continue;
    }

    if (current.length() > 0) {
      lines[lineCount++] = current;
      current = "";
      if (lineCount >= maxLines) {
        truncated = true;
        break;
      }
    }

    if (tft.textWidth(word, font) > maxWidth) {
      while (word.length() > 0 && lineCount < maxLines) {
        String part = "";
        while (word.length() > 0 &&
               tft.textWidth(part + word.substring(0, 1), font) <= maxWidth) {
          part += word.substring(0, 1);
          word.remove(0, 1);
        }
        if (part.length() == 0) {
          part = word.substring(0, 1);
          word.remove(0, 1);
        }
        if (word.length() == 0) current = part;
        else lines[lineCount++] = part;
      }
      if (word.length() > 0) {
        truncated = true;
      }
    } else {
      current = word;
    }
    if (truncated) break;
  }
  if (remaining.length() > 0) truncated = true;
  if (current.length() > 0) {
    if (lineCount < maxLines) lines[lineCount++] = current;
    else truncated = true;
  }
  int totalHeight = lineHeight * lineCount;
  int yStart = cyCenter - totalHeight / 2 + lineHeight / 2;
  tft.setTextColor(color, TFT_BLACK);
  for (int i = 0; i < lineCount; i++) {
    tft.drawString(lines[i], cx, yStart + i * lineHeight, font);
  }
  return !truncated;
}

static String formatDuration(uint32_t totalMs) {
  uint32_t totalSec = totalMs / 1000UL;
  uint32_t minutes = totalSec / 60;
  uint32_t seconds = totalSec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);
  return String(buf);
}

static void drawSpotifyProgress() {
  tft.fillRect(30, 105, 180, 35, TFT_BLACK);
  if (nowPlaying.track.length() == 0) return;

  uint32_t positionMs = nowPlaying.positionMs;
  if (nowPlaying.isPlaying) {
    positionMs += millis() - nowPlaying.positionUpdatedAtMs;
  }
  if (nowPlaying.durationMs > 0 && positionMs > nowPlaying.durationMs) {
    positionMs = nowPlaying.durationMs;
  }
  uint32_t elapsedSec = positionMs / 1000UL;
  uint32_t durationSec = nowPlaying.durationMs / 1000UL;
  char timeStr[32];
  snprintf(timeStr, sizeof(timeStr), "%s / %s",
           formatDuration(elapsedSec * 1000UL).c_str(),
           durationSec > 0 ? formatDuration(durationSec * 1000UL).c_str() : "--:--");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(timeStr, 120, 120, 2);
}

static void drawPlaybackButton(int cx, uint16_t color, uint8_t command) {
  tft.drawCircle(cx, 169, 16, color);
  if (command == PLAYBACK_PREVIOUS) {
    tft.fillTriangle(cx + 5, 161, cx + 5, 177, cx - 4, 169, color);
    tft.fillRect(cx - 8, 161, 2, 16, color);
  } else if (command == PLAYBACK_NEXT) {
    tft.fillTriangle(cx - 5, 161, cx - 5, 177, cx + 4, 169, color);
    tft.fillRect(cx + 6, 161, 2, 16, color);
  } else if (nowPlaying.isPlaying) {
    tft.fillRect(cx - 5, 162, 3, 14, color);
    tft.fillRect(cx + 2, 162, 3, 14, color);
  } else {
    tft.fillTriangle(cx - 4, 161, cx - 4, 177, cx + 7, 169, color);
  }
}

static void drawSpotifyScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (nowPlaying.track.length() == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("no track playing", 120, 105, 2);
  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(truncateToFit(nowPlaying.track, 2, 200), 120, 70, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(truncateToFit(nowPlaying.artist, 2, 200), 120, 95, 2);
    drawSpotifyProgress();
  }

  drawPlaybackButton(70, TFT_WHITE, PLAYBACK_PREVIOUS);
  drawPlaybackButton(120, TFT_WHITE, PLAYBACK_TOGGLE);
  drawPlaybackButton(170, TFT_WHITE, PLAYBACK_NEXT);

  if (nowPlaying.hasAlbumColor) {
    const uint8_t red = (nowPlaying.albumColorRgb >> 16) & 0xFF;
    const uint8_t green = (nowPlaying.albumColorRgb >> 8) & 0xFF;
    const uint8_t blue = nowPlaying.albumColorRgb & 0xFF;
    tft.fillCircle(190, 203, 7, TFT_DARKGREY);
    tft.fillCircle(190, 203, 5, tft.color565(red, green, blue));
  }
}

static void drawLyricsScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (lyricsState.currentLine.length() == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("searching for lyrics...", 120, 120, 2);
    return;
  }

  if (drawWrappedText(lyricsState.currentLine, 120, 120, 4, 200, 4, 30, TFT_WHITE)) {
    return;
  }

  tft.fillScreen(TFT_BLACK);
  if (drawWrappedText(lyricsState.currentLine, 120, 120, 2, 200, 7, 20, TFT_WHITE)) {
    return;
  }

  tft.fillScreen(TFT_BLACK);
  drawWrappedText(lyricsState.currentLine, 120, 120, 1, 200, 8, 15, TFT_WHITE);
}

static void drawManeuverArrow(uint8_t code) {
  const uint16_t color = TFT_CYAN;
  switch (code) {
    case 1:
      tft.fillTriangle(102, 65, 120, 49, 120, 81, color);
      tft.fillRect(116, 59, 24, 12, color);
      break;
    case 2:
      tft.fillTriangle(138, 65, 120, 49, 120, 81, color);
      tft.fillRect(120, 59, 24, 12, color);
      break;
    case 3:
      tft.fillRect(104, 64, 10, 21, color);
      tft.fillRect(104, 55, 29, 10, color);
      tft.fillRect(123, 55, 10, 16, color);
      tft.fillTriangle(118, 68, 138, 68, 128, 86, color);
      break;
    default:
      tft.fillTriangle(120, 45, 104, 63, 136, 63, color);
      tft.fillRect(114, 59, 12, 26, color);
      break;
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

  drawManeuverArrow(navInfo.maneuver);

  tft.setTextDatum(MC_DATUM);
  drawWrappedText(navInfo.instruction, 120, 137, 2, 164, 2, 20, TFT_WHITE);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(truncateToFit(navInfo.distance, 4, 145), 120, 190, 4);
}

static void drawCurrentScreen() {
  // The Spotify and nav screens are meaningless without the phone, so show
  // the "connect to phone" screen instead when disconnected. The main/clock
  // screen keeps working on its own (best-known/persisted time), so it's
  // exempt -- see notifyPhoneConnected()/notifyPhoneDisconnected() below.
  if (!phoneConnected && currentScreen != 0) {
    drawDisconnectedScreen();
    lastDrawnBatteryPercent = batteryPercent;
    return;
  }
  switch (currentScreen) {
    case 0: drawClockScreen(); break;
    case 1: drawSpotifyScreen(); break;
    case 2: drawLyricsScreen(); break;
    case 3: drawNavScreen(); break;
  }
  lastDrawnBatteryPercent = batteryPercent;
}

void initDisplay() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
  // Note: GPIO0 (BOOT) is now owned by input.cpp's OneButton instance, not read here.
}

static void drawBootSplashImage() {
  uint16_t row[240];
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 240; ++x) {
      const uint32_t pixelIndex = (uint32_t)y * 240 + x;
      const uint8_t packed = pgm_read_byte(&bootSplash2bpp[pixelIndex / 4]);
      const uint8_t shift = 6 - (pixelIndex % 4) * 2;
      const uint8_t paletteIndex = (packed >> shift) & 0x03;
      row[x] = pgm_read_word(&bootSplashPalette[paletteIndex]);
    }
    tft.pushImage(0, y, 240, 1, row);
  }
}

void showBootSplash() {
  const uint32_t splashStart = millis();
  drawBootSplashImage();
  const uint32_t elapsed = millis() - splashStart;
  if (elapsed < 1500) delay(1500 - elapsed);
}

void showSplashUntilButtonPress() {
  autoFollowActive = false;
  splashOverlayActive = true;
  drawBootSplashImage();
}

void dismissSplashOverlay() {
  if (!splashOverlayActive) return;
  splashOverlayActive = false;
  forceRedraw = true;
}

bool isSplashOverlayActive() {
  return splashOverlayActive;
}

void cycleScreen() {
  currentScreen = (currentScreen + 1) % 4;
  forceRedraw = true;
  autoFollowActive = false;  // user took manual control -- stop auto-jumping for this session
}

void previousScreen() {
  currentScreen = (currentScreen + 3) % 4;
  forceRedraw = true;
  autoFollowActive = false;
}

bool isSpotifyScreenActive() {
  return currentScreen == 1;
}

bool isMainScreenActive() {
  return currentScreen == 0;
}

bool isDisconnectedScreenActive() {
  return !phoneConnected && currentScreen != 0;
}

void setDisplayBacklight(bool enabled) {
  digitalWrite(TFT_BL, enabled ? TFT_BACKLIGHT_ON : !TFT_BACKLIGHT_ON);
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
  if (splashOverlayActive) return;

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

  if (batteryPercent != lastDrawnBatteryPercent &&
      (showingDisconnected || currentScreen == 0)) {
    needRedraw = true;
  }

  if (showingDisconnected && batteryPercent != lastDrawnBatteryPercent) {
    needRedraw = true;
  } else if (currentScreen == 0 && baseEpoch != 0) {
    uint32_t localEpoch = currentLocalEpoch();
    uint32_t currentSecond = localEpoch;
    uint32_t currentDay = localEpoch / 86400UL;
    if (currentDay != lastClockDay) {
      needRedraw = true;
      lastClockDay = currentDay;
    } else if (currentSecond != lastClockSecond) {
      drawClockTime();
      lastClockSecond = currentSecond;
    }
    if (batteryPercent != lastDrawnBatteryPercent) needRedraw = true;
    lastClockSecond = currentSecond;
  }

  if (!showingDisconnected && currentScreen == 1 && nowPlaying.dirty) {
    needRedraw = true;
    nowPlaying.dirty = false;
  } else if (!showingDisconnected && currentScreen == 2 && lyricsState.dirty) {
    needRedraw = true;
    lyricsState.dirty = false;
  } else if (!showingDisconnected && currentScreen == 3 && navInfo.dirty) {
    needRedraw = true;
    navInfo.dirty = false;
  }

  if (!showingDisconnected && currentScreen == 1 && nowPlaying.isPlaying &&
      nowPlaying.track.length() > 0) {
    uint32_t second = (nowPlaying.positionMs + millis() - nowPlaying.positionUpdatedAtMs) / 1000UL;
    if (second != lastSpotifySecond) {
      drawSpotifyProgress();
      lastSpotifySecond = second;
    }
  }

  if (needRedraw) {
    drawCurrentScreen();
    forceRedraw = false;
    if (currentScreen == 0 && baseEpoch != 0) {
      uint32_t localEpoch = currentLocalEpoch();
      lastClockSecond = localEpoch;
      lastClockDay = localEpoch / 86400UL;
    }
    if (currentScreen == 1) {
      lastSpotifySecond = (nowPlaying.positionMs + millis() - nowPlaying.positionUpdatedAtMs) / 1000UL;
    }
  }
}
