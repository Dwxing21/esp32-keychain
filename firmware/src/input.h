#pragma once

// Sets up the BOOT button (GPIO0) via OneButton.
void initInput();

// Call every loop() iteration to let OneButton evaluate click timing.
void updateInput();
