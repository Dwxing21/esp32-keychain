#pragma once

// Initializes the TFT and the BOOT-button menu cycling.
void initDisplay();

// Call every loop() iteration. Internally throttled: only redraws the clock
// once a second and the other screens when their underlying state changes.
void updateDisplay();

// Advances to the next screen (clock -> Spotify -> nav -> clock...). Called by the
// button handler in input.cpp.
void cycleScreen();

// True while the Spotify screen is showing, so the button handler knows whether a
// click/double-click should control playback instead of cycling screens.
bool isSpotifyScreenActive();

// Called by ble_service.cpp's connect/disconnect callbacks.
void notifyPhoneConnected();
void notifyPhoneDisconnected();
