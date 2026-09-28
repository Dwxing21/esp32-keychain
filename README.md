# ESP32-S3 smartwatch: clock + Spotify controls + Maps nav

Two parts:
- `firmware/` — PlatformIO project for the Waveshare ESP32-S3-LCD-1.28 (GC9A01 round display, no touch). Runs a BLE GATT server, draws three screens (clock, Spotify, navigation), and reads the BOOT button for playback control / screen cycling.
- `android-app/` — Android Studio project. Reads Spotify's now-playing info and Google Maps' navigation notifications, and relays both to the watch over BLE. Also receives playback commands (play/pause, next) from the watch's BOOT button and forwards them to Spotify.

## How data flows

```
Spotify app  ---MediaSession--->  \
                                    Android companion app  ---BLE--->  ESP32-S3 watch
Google Maps  ---notification--->  /                        <--BLE---  (playback commands)
```

The watch never talks to WiFi, Spotify's API, or Google's API directly — the phone does all of that and just pushes simple text/state over BLE. This also means the watch gets its clock from the phone (over BLE) rather than NTP, so no WiFi credentials are needed on the watch at all.

## Firmware setup (PlatformIO)

1. Install the [PlatformIO extension](https://platformio.org/install/ide?install=vscode) for VS Code (or use the PlatformIO CLI).
2. Open the `firmware/` folder as a PlatformIO project.
3. Connect the board over USB-C, then **Build & Upload**.
4. Open the serial monitor (115200 baud) — you should see `[BLE] GATT server up, advertising as 'ESP32 Smartwatch'`.

**If the display stays blank:** double-check the pins in `include/User_Setup.h` against your board's schematic (linked in that file) — Waveshare has shipped slightly different pinouts across batches.

**Screen behavior:**
- The cycle is now four screens: main/clock -> Spotify -> lyrics -> navigation -> back to main.
- Boots to the main/clock screen. If music is already playing or navigation is already active by the time the phone connects, it jumps there once automatically (lyrics is never auto-jumped to -- it's a secondary view you reach by cycling) -- after that (or after your first button press), it's fully manual.
- If the phone isn't connected, the Spotify and nav screens show a "Connect to phone" message with a battery icon instead of stale/empty content. The clock screen is exempt from this -- it keeps working on its own using the last-known or persisted time, since a watch that can't show the time without a nearby phone defeats the point. (If you actually want the clock replaced too, that's a one-line change -- just ask.)
- The clock shows the last-synced time immediately on boot instead of a blank screen, saved to flash via NVS (`time_store.cpp`) each time the phone syncs. There's no battery-backed RTC chip on this board, so this doesn't account for how long the device was powered off -- it's a best-effort "don't show nothing" measure, corrected within seconds once the phone reconnects. The phone also re-syncs every 30 minutes while connected (not just once), to catch clock drift or a DST change.
- Date is shown as day-month-year (e.g. `25-09-2026`).
- The battery icon reads from `battery.cpp`, which uses GPIO1 (this board's confirmed BAT_ADC pin) to estimate a percentage. The pin is confirmed, but the voltage-divider ratio (`DIVIDER_RATIO`, currently assumed 2.0) and the empty/full voltage thresholds haven't been verified against a multimeter reading of your actual battery -- worth checking once you're testing, since a wrong ratio means a wrong percentage even though the pin itself is right.

**BOOT button behavior:**
- Clock/nav screens: click or double-click cycles to the next screen.
- Spotify screen: click = play/pause, double-click = next track, long-press = leave the screen (there's no button gesture for "previous track" yet — wire a second button to a spare GPIO if you want one, and I can add the firmware/BLE/app-side handling for it).

## Android app setup

1. Open `android-app/` in Android Studio, let it sync Gradle.
2. Run it on a phone (not an emulator — you need real Bluetooth and the Spotify/Maps apps installed).
3. In the app: tap **Grant notification access**, and enable "Smartwatch notification reader" in the system settings page that opens. This is what lets it read Spotify's media session and Google Maps' notifications — there's no runtime permission dialog for this, Android requires the manual settings toggle.
4. Tap **Connect to watch** and accept the Bluetooth permission prompts.
5. Play something in Spotify — the watch's Spotify screen should update. Start turn-by-turn navigation in Google Maps — the watch's nav screen should update.

**Keeping it connected:** Android will eventually kill the notification listener / BLE connection in the background on many phones unless you disable battery optimization for this app (Settings → Apps → Smartwatch Companion → Battery → Unrestricted). A proper foreground service with a persistent notification would help here too — this skeleton doesn't include one yet.

## Known limitations / things worth knowing before you build on this

- **Google Maps nav parsing is best-effort.** It reads whatever text Maps puts in its persistent navigation notification's title/text fields. Google can change that layout without notice, which would break the parsing — there's no official API for this.
- **Distance units follow Google Maps' own setting, not ours.** The nav distance shown on the watch (e.g. "90 m" vs "300 ft") is relayed verbatim from Maps' notification text — our code never sees a raw number, just a string, so it can't convert units itself. To get metric on the watch, set it in Google Maps' own app settings (Settings → Navigation settings → Distance units), or your phone's region.
- **No "previous track" button gesture** — see the BOOT button section above.
- **No reconnection UI feedback** beyond logcat — the app will keep retrying the BLE scan every 3 seconds if disconnected, but the status text on screen doesn't reflect that yet.
- iOS is not supported for the Spotify-reading half of this (Apple doesn't expose background media session data the way Android does) — this whole approach is Android-only.

## Lyrics

A dedicated screen in the cycle shows the current lyric line, synced to playback position. A few things worth knowing:

- **Spotify has no public lyrics API**, so this doesn't come from Spotify at all. It uses [lrclib.net](https://lrclib.net) -- a free, community-run lyrics database with synced (per-line timestamped) lyrics, no API key needed. `LyricsFetcher.kt` handles the lookup and LRC parsing.
- On a track change, the phone app clears the old lyrics immediately, fetches new ones in the background (network call, so there's a brief "no lyrics" flash even for tracks that do have them), and starts polling playback position every 500ms to catch line changes.
- **Coverage is inconsistent** -- mainstream tracks are usually covered, obscure or very new releases often aren't. When nothing's found, the screen just says "No lyrics found" rather than erroring.
- This is the first thing in the project that needs phone internet access, so `INTERNET` permission was added to the manifest. It only talks to lrclib.net -- no account, no auth, no data sent beyond track/artist/duration for the lookup.
- The word-wrap on the watch (`drawWrappedText` in `display.cpp`) shows up to 3 lines before truncating -- fine for a single lyric line, but a good thing to know if a source ever returns unusually long lines.

## UI icons and screen previews

`firmware/src/icons.h` has 19 generated icon bitmaps (play/pause/prev/next, four nav arrows, 5-level battery in two sizes, a disconnected-Bluetooth glyph) wired into `display.cpp` via `tft.drawXBitmap()`, replacing the earlier placeholder circles/text. `firmware/previews/` has renders of all six screen states (clock, disconnected, Spotify playing/paused, nav straight/left) at actual layout -- generated by simulating the exact draw calls and coordinates from `display.cpp`, masked to the round display, so they reflect real positioning, not just mockups. Worth knowing:
- The preview font is a stand-in (DejaVuSansMono) for TFT_eSPI's built-in GLCD font -- close in spirit, not pixel-identical, so expect minor differences on real hardware.
- Rendering these caught a real bug: a long track title or nav instruction would have run off the screen entirely. `display.cpp` now has a `truncateToFit()` helper that measures text width and adds "..." before it overflows -- applied to track name, artist, and nav instruction.
- If you design your own versions in Lopaka later, match these exact pixel sizes so they drop into the existing layout: play/pause 32x32, prev/next 28x28, nav arrows 40x40, battery 26x13 (small) and 50x24 (large), Bluetooth glyph 24x24.

## Power source

Your exact board (the plain, non-touch ESP32-S3-LCD-1.28) has a real onboard charging circuit — an **ETA6096 Li-ion charge manager** — and an **MX1.25 2-pin battery connector**. That means you can plug a 3.7V single-cell LiPo straight into that header, and USB-C both powers the board and charges the battery at the same time. No extra charging module needed.

What to get:
- A **3.7V single-cell LiPo with an MX1.25 (JST 1.25mm pitch) 2-pin connector**, correct polarity (check the board's silkscreen "+"/"-" against the wire colors before plugging in — reversed polarity on a LiPo is a real fire risk).
- Capacity: 100–200mAh is a sensible range for something this small. `case/keychain_case.scad` has a battery pocket sized for a ~25×20×5mm cell (roughly a 302025 or similar); a 502030-size cell would need the pocket resized larger.
- Watch battery life expectations: with the display on and BLE connected continuously, runtime will likely be a few hours, not days, on a 100–150mAh cell. Adding a sleep/dim mode (mentioned as a "still to do" in the firmware notes) would meaningfully extend that.

Don't use a LiPo without a protection circuit built in (most pre-wired ones sold for hobby electronics have one) — the ETA6096 handles charge control, but cell-level over-discharge/short protection should still be present on the battery itself.

## 3D-printed keychain case

`case/keychain_case.scad` is a parametric OpenSCAD design: a two-part shell (body + lid) that holds the board, has a battery pocket, cutouts for the USB-C port and BOOT button, and a keychain loop. Rendered STLs (`case/keychain_case_body.stl`, `case/keychain_case_lid.stl`) and preview renders (`case/preview_body.png`, `case/preview_lid.png`) are included so you can see the design without opening OpenSCAD.

**Read the comment block at the top of the .scad file before printing.** The display opening (32.4mm) is solid — that's just what "1.28 inch round" means — but the PCB's outer diameter and exactly where the USB-C port / BOOT button / battery connector sit around the edge are estimates, since Waveshare only publishes a PDF drawing, not machine-readable dimensions. Measure your actual board and adjust the variables at the top of the file (all clearly marked) before committing filament to a full-quality print. A quick low-infill draft print of just the body is a cheap way to test the fit first.

To tweak and re-render: install [OpenSCAD](https://openscad.org/downloads.html), open `keychain_case.scad`, change `part = "body"` to `"lid"` or `"both"`, adjust the marked variables, and render (F6) then export as STL (F7).

