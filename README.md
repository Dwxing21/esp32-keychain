# ESP32-S3 Smartwatch

An open-source smartwatch project for the [Waveshare ESP32-S3-Touch-LCD-1.28](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28). The watch displays the time, Spotify playback, synced lyrics, and Google Maps turn instructions. An Android companion app reads phone state and relays it to the watch over Bluetooth Low Energy (BLE).

> **Status:** Firmware and Android app build from source. Hardware features and phone-specific background behavior may vary; see [Known limitations](#known-limitations).

## Project contents

| Directory | Description |
| --- | --- |
| [`firmware/`](firmware/) | PlatformIO / Arduino firmware for the watch |
| [`android-app/`](android-app/) | Android Studio companion app |

## Contents

- [Features](#features)
- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Build and upload firmware](#build-and-upload-firmware)
- [Build and install the Android app](#build-and-install-the-android-app)
- [Controls](#controls)
- [Known limitations](#known-limitations)
- [Lyrics](#lyrics)
- [Battery and charging](#battery-and-charging)

## Features

- Clock synchronized from the phone.
- BLE-connected Spotify controls and playback status.
- Synced lyrics from LRCLIB, with Greek-to-Greeklish display conversion.
- Google Maps navigation instructions and maneuver arrows.
- Touchscreen swipe navigation and Spotify playback buttons.
- Estimated battery level.

## How data flows

```
Spotify app  ---MediaSession--->  \
                                    Android companion app  ---BLE--->  ESP32-S3 watch
Google Maps  ---notification--->  /                        <--BLE---  (playback commands)
```

The watch does not connect to Wi-Fi or call Spotify/Google APIs. The phone reads media and navigation state and sends relevant data over BLE. The watch receives its time from the phone, so Wi-Fi credentials are not needed.

## Hardware

- Waveshare ESP32-S3-Touch-LCD-1.28
- 240 × 240 GC9A01 round display
- CST816S capacitive touch controller
- Optional protected, single-cell 3.7 V LiPo compatible with the board connector

The display and touch pin configuration is in [`firmware/include/User_Setup.h`](firmware/include/User_Setup.h). Verify the board revision and battery polarity before powering the hardware.

## Build and upload firmware

1. Install the [PlatformIO extension](https://platformio.org/install/ide?install=vscode) for VS Code or the [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html).
2. Open `firmware/` as a PlatformIO project.
3. Connect the watch over USB-C and build/upload the `esp32-s3-lcd-128` environment.
4. Open the serial monitor at 115200 baud. The firmware advertises as `ESP32 Smartwatch`.

CLI commands, run from `firmware/`:

```powershell
pio run -e esp32-s3-lcd-128
pio run -e esp32-s3-lcd-128 --target upload
pio device monitor --baud 115200
```

**If the display stays blank:** double-check the pins in `include/User_Setup.h` against your board's schematic (linked in that file) — Waveshare has shipped slightly different pinouts across batches.

## Controls

### Touch gestures
- Swipe left to advance through the screens, wrapping from navigation back to the clock; swipe right to go back, wrapping from the clock to navigation.
- Tap the previous, play/pause, or next buttons on the Spotify screen to control playback.
- Double-tap the clock to show the splash overlay; tap again to dismiss it.
- Hold the clock for 800 ms to enter deep sleep. Hold BOOT to wake; BLE disconnects while asleep.
- The BOOT button remains available as a backup for screen and playback controls.

### Screen behavior
- The cycle is now four screens: main/clock -> Spotify -> lyrics -> navigation -> back to main.
- Boots to the main/clock screen. If music is already playing or navigation is already active by the time the phone connects, it jumps there once automatically (lyrics is never auto-jumped to -- it's a secondary view you reach by cycling) -- after that (or after your first button press), it's fully manual.
- Shows a 1.5-second splash at startup and after waking from BOOT-button deep sleep. It uses the first frame of `animation_export.mp4`; the provided three-second MP4 has identical sampled frames, so the displayed splash is a still image rather than a video animation.
- If the phone isn't connected, the Spotify and nav screens show a "Connect to phone" message with a battery icon instead of stale/empty content. The clock screen is exempt from this -- it keeps working on its own using the last-known or persisted time, since a watch that can't show the time without a nearby phone defeats the point. (If you actually want the clock replaced too, that's a one-line change -- just ask.)
- Static watch screens redraw only when their displayed data changes. The clock updates just its time field each second; the disconnected screen refreshes when the battery percentage changes.
- The clock shows the last-synced time immediately on boot instead of a blank screen, saved to flash via NVS (`time_store.cpp`) each time the phone syncs. There's no battery-backed RTC chip on this board, so this doesn't account for how long the device was powered off -- it's a best-effort "don't show nothing" measure, corrected within seconds once the phone reconnects. The phone also re-syncs every 30 minutes while connected (not just once), to catch clock drift or a DST change.
- Date is shown as day-month-year (e.g. `25-09-2026`).
- The battery icon reads from `battery.cpp`, which uses GPIO1 (this board's confirmed BAT_ADC pin) to estimate a percentage. The ADC is sampled nine times, median-filtered, calibrated with `analogReadMilliVolts`, and smoothed over time. The Waveshare schematic specifies a 3:1 battery-to-ADC divider, so `BATTERY_DIVIDER_RATIO` defaults to 3.0. Percentage uses an approximate single-cell LiPo voltage curve, not a fuel gauge, so load, temperature, and cell age still affect accuracy. The watch logs the filtered battery voltage and estimated percentage to the serial monitor every five seconds. Charging state is not reported because firmware has no confirmed charger-status input; voltage trend alone is not reliable enough to label as charging.
- The Spotify screen shows a small accent dot based on the dominant color sampled from album artwork supplied by the active media session. If no artwork is available, the dot is hidden.
- Main/clock screen: touch-and-hold or hold BOOT to turn off the display and enter deep sleep, which disconnects BLE. Hold BOOT to wake. This is low-power sleep, not a physical battery disconnect.

### BOOT button behavior
- Clock/nav screens: click or double-click cycles to the next screen.
- Spotify screen: click = play/pause, double-click = next track, long-press = leave the screen.

## Build and install the Android app

The app requires Android 8.0 (API 26) or newer. Use a physical phone with Bluetooth, Spotify, and Google Maps installed; an emulator cannot provide the full device workflow.

To build a debug APK:

```powershell
cd android-app
.\gradlew.bat :app:assembleDebug
```

The APK is created at `android-app/app/build/outputs/apk/debug/app-debug.apk`. Alternatively, open `android-app/` in Android Studio and use **Build → Build Bundle(s) / APK(s) → Build APK(s)**.

On first launch:

1. Tap **Grant / manage notification access** and enable **Smartwatch notification reader** in Android settings. Android requires the user to grant this special access manually.
2. Return to the app and tap **Connect to watch**, then accept the Bluetooth permission prompts.
3. Start playback in Spotify or turn-by-turn navigation in Google Maps.

Use **Disconnect from watch** to end the BLE connection. On some Android devices, battery management can stop background listener/BLE activity. If the app reports that notification access is enabled but the listener is disconnected, use its notification-access button to retry or open Android settings and toggle this app's access off and back on. Reconnection behavior depends on Android and device-vendor policies.

When BLE connects or reconnects, the app resends the current Spotify state and any active Google Maps navigation notification, so updates that happened before the GATT connection is ready are not lost.

**Keeping it connected:** Android may stop listener or BLE activity in the background, especially under aggressive vendor battery management. If needed, set the app's battery usage to **Unrestricted** in Android app settings. A foreground service with a persistent notification is not currently implemented.

## Known limitations

- **Google Maps nav parsing is best-effort.** It reads whatever text Maps puts in its persistent navigation notification's title/text fields. Google can change that layout without notice, which would break the parsing — there's no official API for this.
- **Distance units follow Google Maps' own setting, not ours.** The nav distance shown on the watch (e.g. "90 m" vs "300 ft") is relayed verbatim from Maps' notification text — our code never sees a raw number, just a string, so it can't convert units itself. To get metric on the watch, set it in Google Maps' own app settings (Settings → Navigation settings → Distance units), or your phone's region.
- **Greek Maps instructions are shown in Greeklish on the watch.** Navigation text is transliterated for the watch font, and common Greek left/right/U-turn instructions are mapped to the corresponding arrow.
- Notification-listener reconnection is controlled by Android. On some devices, access may show as enabled while the service is disconnected; manual off/on toggling in system settings may be required.
- Touch input has not been confirmed on every board revision. If touch gestures do not work, check the CST816S I²C wiring and board pinout.
- iOS is not supported for the Spotify-reading half of this (Apple doesn't expose background media session data the way Android does) — this whole approach is Android-only.

## Lyrics

A dedicated screen in the cycle shows the current lyric line, synced to playback position. A few things worth knowing:

- **Spotify has no public lyrics API**, so this doesn't come from Spotify at all. It uses [lrclib.net](https://lrclib.net) -- a free, community-run lyrics database with synced (per-line timestamped) lyrics, no API key needed. `LyricsFetcher.kt` handles the lookup and LRC parsing.
- On a track change, the phone app shows a searching state while looking up synced lyrics in the background, then displays the lyric or "No lyrics found". Search results are checked against track title, artist, and duration to reduce wrong-song matches. Greek titles are normalized and searched in both Greek and Greeklish transliteration. Greek lyric lines are converted to Greeklish on the watch because its built-in font does not reliably include Greek Unicode glyphs.
- **Coverage is inconsistent** -- mainstream tracks are usually covered, obscure or very new releases often aren't. When nothing's found, the screen just says "No lyrics found" rather than erroring.
- This is the first thing in the project that needs phone internet access, so `INTERNET` permission was added to the manifest. It only talks to lrclib.net -- no account, no auth, no data sent beyond track/artist/duration for the lookup.
- The word-wrap on the watch (`drawWrappedText` in `display.cpp`) tries progressively smaller fonts to fit unusually long lyric lines.

## UI icons and screen previews

`firmware/src/icons.h` contains navigation, Bluetooth, and legacy playback XBM bitmaps. Battery icons are drawn from TFT primitives for a clean outline and proportional fill at both sizes. The preview images may predate recent UI changes; verify the firmware on-device for the current layout. Worth knowing:
- The preview font is a stand-in (DejaVuSansMono) for TFT_eSPI's built-in GLCD font -- close in spirit, not pixel-identical, so expect minor differences on real hardware.
- Long track and artist names are truncated to fit. Navigation instructions wrap across a bounded number of lines, and the distance string is limited to the available display width.
- If you design your own versions in Lopaka later, match these exact pixel sizes so they drop into the existing layout: play/pause 32x32, prev/next 28x28, nav arrows 40x40, battery 26x13 (small) and 50x24 (large), Bluetooth glyph 24x24.

## Battery and charging

The Waveshare board has an onboard single-cell LiPo charging circuit and a 2-pin battery connector. Follow the board documentation and silkscreen for connector type and polarity. Charging state is not reported by the firmware because a usable charger-status input has not been confirmed.

What to get:
- A **protected 3.7V single-cell LiPo** with a connector compatible with the board. Verify polarity against the board's silkscreen before connecting; reversed polarity can damage the board or battery.
- The current single-cell LiPo is 40×30×5mm and rated 550mAh. Runtime depends on actual watch current draw, display brightness, and BLE activity; it has not been measured for this cell. Measure a full-charge-to-low-battery run to establish a reliable estimate.

Use a battery with appropriate cell-level protection. The onboard charger does not replace battery over-discharge and short-circuit protection.
