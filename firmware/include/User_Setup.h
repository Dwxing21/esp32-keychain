// TFT_eSPI setup for the Waveshare ESP32-S3-LCD-1.28 (GC9A01, 240x240 round IPS)
//
// These pins are the community-confirmed values for this exact board
// (see https://github.com/Bodmer/TFT_eSPI/discussions/3283). If your display
// stays blank, double check against the schematic at:
// https://files.waveshare.com/wiki/ESP32-S3-LCD-1.28/Esp32-s3-lcd-.128-sch.pdf

#define USER_SETUP_ID 303
#define USER_SETUP_INFO "ESP32-S3-LCD-1.28"

#define GC9A01_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define TFT_MOSI 11
#define TFT_SCLK 10
#define TFT_CS    9
#define TFT_DC    8
#define TFT_RST  12
#define TFT_BL   40
#define TFT_BACKLIGHT_ON HIGH

// Required on the S3 when using GPIO pins outside the default FSPI range
#define USE_HSPI_PORT

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// Start conservative; the board has been reported to run at 80MHz if this proves stable
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
