#pragma once

// Call once in setup().
void initBattery();

// Call every loop() iteration; internally throttled to check every few seconds.
void updateBattery();
