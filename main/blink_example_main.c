#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
// forward-declared by your driver .h
esp_err_t esp_lcd_new_panel_st7365(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

#define PIN_NUM_MOSI 13
#define PIN_NUM_SCLK 10
#define PIN_NUM_CS   9
#define PIN_NUM_DC   11
#define PIN_NUM_RST  46

// set these to your glass
#define LCD_W 320
#define LCD_H 480

// pack 5-6-5
static inline uint16_t RGB565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void app_main(void)
{
    esp_err_t err;

    // 1) SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,          // most TFTs don't use MISO
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 2 * 40, // 40 lines * 2 bytes/pixel
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 2) Panel IO over SPI (command/data split by DC)
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 40 * 1000 * 1000,   // drop to 26 MHz if you see garbage
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((spi_host_device_t)SPI2_HOST, &io_config, &io));

    // 3) Create your ST7365 panel
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t devcfg = {
        .reset_gpio_num = PIN_NUM_RST,
        .bits_per_pixel = 16,                        // we’re testing RGB565 first
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,  // try BGR if colors look swapped
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,      // try LITTLE if colors look garbled
        .flags = { .reset_active_high = false },
        .vendor_config = NULL,                       // you hardcoded width/height in the driver
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7365(io, &devcfg, &panel));

    // 4) Reset + init
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    // 5) --- One-line test ---
    
    static uint16_t line[LCD_W]; // stays in internal RAM; fine for a quick test
    
    for (int x = 0; x < LCD_W; x++)
    {
        if(x < 40) line[x] = RGB565(255, 255, 255);
        else if(x > LCD_W/2 - 20 && x < LCD_W/2 + 20) line[x] = RGB565(255, 255, 255);
        else if(x >= LCD_W - 40) line[x] = RGB565(255, 255, 255);
        else line[x] = RGB565(0, 0, 0);
    }
    for (int y=0; y<LCD_H; y++) ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y+1, line));
   
    // for (int x = 0; x < LCD_W; x++) line[x] = RGB565(255, 0, 0);
    // ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, 10, line));

    // for (int x = 0; x < LCD_W; x++) line[x] = RGB565(0, 255, 0);
    // ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, (LCD_H/2) - 5, LCD_W, (LCD_H/2) + 5, line));

    // for (int x = 0; x < LCD_W; x++) line[x] = RGB565(0, 0, 255);
    // ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, LCD_H-10, LCD_W, LCD_H-1, line));

    // // 6) Fill the screen with bars (optional but helpful)
    // for (int y = 0; y < LCD_H; y++) {
    //     uint16_t color;
    //     if (y < LCD_H/3)       color = RGB565(255,50,50);     // red
    //     else if (y < 2*LCD_H/3) color = RGB565(50,255,50);    // green
    //     else                    color = RGB565(50,50,255);    // blue
    //     for (int x = 0; x < LCD_W; x++) line[x] = color;
    //     ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y+1, line));
    // }

    // sit there
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}