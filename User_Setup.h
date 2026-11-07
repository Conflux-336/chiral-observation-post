// ============================================================================
//  TFT_eSPI  User_Setup.h
//  目标板：ESP32-1732S019N (ESP32-S3 N16R8 + 板载 1.9" ST7789 170x320, 无触摸)
//  引脚依据厂商 PlatformIO demo（OttoMeister/ESP32-1732S019）
//  原始文件已备份为 User_Setup.h.orig.bak
// ============================================================================

#define USER_SETUP_INFO "ESP32-1732S019N 170x320"

#define ST7789_DRIVER

#define TFT_WIDTH  170
#define TFT_HEIGHT 320

#define TFT_MISO  -1
#define TFT_MOSI  13
#define TFT_SCLK  12
#define TFT_CS    10
#define TFT_DC    11
#define TFT_RST    1
#define TFT_BL    14

#define TFT_BACKLIGHT_ON HIGH

#define USE_HSPI_PORT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  16000000

// 若画面出现“白底 / 颜色反相”，把下面这行打开
//#define TFT_INVERSION_ON

#define LOAD_GLCD
#define LOAD_FONT2
#define SMOOTH_FONT
