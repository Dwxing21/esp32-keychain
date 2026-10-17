// TFT_eSPI setup for the Waveshare ESP32-S3-Touch-LCD-1.28 (GC9A01, 240x240 IPS)
//
// Pin mapping follows the touch-board schematic:
// https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28/ESP32-S3-Touch-LCD-1.28-Sch.pdf

#define USER_SETUP_ID 303
#define USER_SETUP_INFO "ESP32-S3-Touch-LCD-1.28"

#define GC9A01_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define TFT_MOSI 11
#define TFT_SCLK 10
#define TFT_CS    9
#define TFT_DC    8
#define TFT_MISO 12
#define TFT_RST  14
#define TFT_BL    2
#define TFT_BACKLIGHT_ON HIGH

// Select SPI3 explicitly; TFT_eSPI's ESP32-S3 register access expects this bus.
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
