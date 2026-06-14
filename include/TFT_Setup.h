// ── Driver ──────────────────────────────────
#define ST7789_DRIVER

// ── Resolution ──────────────────────────────
#define TFT_WIDTH   172
#define TFT_HEIGHT  320

// ── SPI Port (ESP32-S3 এর জন্য MANDATORY) ──
#define USE_HSPI_PORT       //  crash 

// ── Pins ────────────────────────────────────
#define TFT_MOSI   4
#define TFT_SCLK   44
#define TFT_CS    3
#define TFT_DC     6
#define TFT_RST    5
//#define TFT_BL     15      

// ── Speed ───────────────────────────────────
#define SPI_FREQUENCY       80000000
#define SPI_READ_FREQUENCY   6000000

// ── Fonts (optional but useful) ─────────────
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
