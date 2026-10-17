#pragma once

// Sets up the CST816S touch controller and BOOT button (GPIO0).
void initInput();

// Poll touch and the BOOT button. Call every loop() iteration.
void updateInput();
