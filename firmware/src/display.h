#pragma once

// Initializes the TFT display.
void initDisplay();

// Shows the image extracted from the provided video for 1.5 seconds.
void showBootSplash();
// Shows the splash overlay until input.cpp dismisses it on touch or BOOT.
void showSplashUntilButtonPress();
void dismissSplashOverlay();
bool isSplashOverlayActive();

// Call every loop() iteration. Internally throttled: only redraws the clock
// once a second and the other screens when their underlying state changes.
void updateDisplay();

// Advances or reverses the screen order. Called by input.cpp.
void cycleScreen();
void previousScreen();

// True while the Spotify screen is showing, so the button handler knows whether a
// click/double-click should control playback instead of cycling screens.
bool isSpotifyScreenActive();
bool isMainScreenActive();
bool isDisconnectedScreenActive();
void setDisplayBacklight(bool enabled);

// Called by ble_service.cpp's connect/disconnect callbacks.
void notifyPhoneConnected();
void notifyPhoneDisconnected();
