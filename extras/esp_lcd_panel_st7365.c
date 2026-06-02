/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"

#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_compiler.h"

#define st7365_CMD_RAMCTRL               0xb0
#define st7365_DATA_LITTLE_ENDIAN_BIT    (1 << 3)

static const char *TAG = "lcd_panel.st7365";

static esp_err_t panel_st7365_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7365_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7365_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7365_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data);
static esp_err_t panel_st7365_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7365_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7365_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7365_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7365_disp_on_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t panel_st7365_sleep(esp_lcd_panel_t *panel, bool sleep);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint16_t width, height;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;    // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val;    // save current value of LCD_CMD_COLMOD register
    uint8_t ramctl_val_1;
    uint8_t ramctl_val_2;
} st7365_panel_t;

esp_err_t
esp_lcd_new_panel_st7365(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                         esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    st7365_panel_t *st7365 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    // leak detection of st7365 because saving st7365->base address
    ESP_COMPILER_DIAGNOSTIC_PUSH_IGNORE("-Wanalyzer-malloc-leak")
    st7365 = calloc(1, sizeof(st7365_panel_t));
    ESP_GOTO_ON_FALSE(st7365, ESP_ERR_NO_MEM, err, TAG, "no mem for st7365 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7365->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7365->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7365->colmod_val = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        st7365->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7365->ramctl_val_1 = 0x00;
    st7365->ramctl_val_2 = 0xf0;    // Use big endian by default
    if ((panel_dev_config->data_endian) == LCD_RGB_DATA_ENDIAN_LITTLE) {
        // Use little endian
        st7365->ramctl_val_2 |= st7365_DATA_LITTLE_ENDIAN_BIT;
    }

    st7365->io = io;
    st7365->fb_bits_per_pixel = fb_bits_per_pixel;
    st7365->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7365->reset_level = panel_dev_config->flags.reset_active_high;
    st7365->width = 320;
    st7365->height = 480;
    st7365->base.del = panel_st7365_del;
    st7365->base.reset = panel_st7365_reset;
    st7365->base.init = panel_st7365_init;
    st7365->base.draw_bitmap = panel_st7365_draw_bitmap;
    st7365->base.invert_color = panel_st7365_invert_color;
    st7365->base.set_gap = panel_st7365_set_gap;
    st7365->base.mirror = panel_st7365_mirror;
    st7365->base.swap_xy = panel_st7365_swap_xy;
    st7365->base.disp_on_off = panel_st7365_disp_on_off;
    st7365->base.disp_sleep = panel_st7365_sleep;
    *ret_panel = &(st7365->base);
    ESP_LOGD(TAG, "new st7365 panel @%p", st7365);

    return ESP_OK;

err:
    if (st7365) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7365);
    }
    return ret;
    ESP_COMPILER_DIAGNOSTIC_POP("-Wanalyzer-malloc-leak")
}

static esp_err_t panel_st7365_del(esp_lcd_panel_t *panel)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);

    if (st7365->reset_gpio_num >= 0) {
        gpio_reset_pin(st7365->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7365 panel @%p", st7365);
    free(st7365);
    return ESP_OK;
}

static esp_err_t panel_st7365_reset(esp_lcd_panel_t *panel)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7365->io;

    // perform hardware reset
    if (st7365->reset_gpio_num >= 0) {
        gpio_set_level(st7365->reset_gpio_num, st7365->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7365->reset_gpio_num, !st7365->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG,
                            "io tx param failed");
        vTaskDelay(pdMS_TO_TICKS(120)); // spec, wait at least 5m before sending new command
    }

    return ESP_OK;
}

static esp_err_t panel_st7365_init(esp_lcd_panel_t *panel)
{
    st7365_panel_t *st = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st->io;

    // 1) Exit sleep (panel powers up in sleep)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(120));   // be generous

    // 2) Pixel format: 16-bit RGB565
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]){ st->colmod_val }, 1), TAG, "colmod");
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3) Memory access control (orientation + BGR)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){ st->madctl_val }, 1), TAG, "madctl");
    vTaskDelay(pdMS_TO_TICKS(10));

    // 4) RAMCTRL (endianness / data order) — okay to keep simple first
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, st7365_CMD_RAMCTRL,
        (uint8_t[]){ st->ramctl_val_1, st->ramctl_val_2 }, 2), TAG, "ramctrl");

    // 5) Program full address window once (0..w-1, 0..h-1), including any configured gaps
    uint16_t x0 = st->x_gap;
    uint16_t x1 = st->x_gap + st->width  - 1;
    uint16_t y0 = st->y_gap;
    uint16_t y1 = st->y_gap + st->height - 1;
    uint8_t caset[4] = { x0>>8, x0&0xFF, x1>>8, x1&0xFF };
    uint8_t raset[4] = { y0>>8, y0&0xFF, y1>>8, y1&0xFF };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, caset, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, raset, 4), TAG, "raset");

    // 6) (Optional) Inversion — leave off unless your glass expects it
    // ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_INVON, NULL, 0), TAG, "invon");

    // 7) Display ON — don’t forget this
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0), TAG, "dispon");
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

static esp_err_t panel_st7365_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7365->io;

    x_start += st7365->x_gap;
    x_end += st7365->x_gap;
    y_start += st7365->y_gap;
    y_end += st7365->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "io tx param failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * st7365->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "io tx color failed");

    return ESP_OK;
}

static esp_err_t panel_st7365_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7365->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_st7365_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7365->io;
    if (mirror_x) {
        st7365->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7365->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7365->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7365->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7365->madctl_val
    }, 1), TAG, "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_st7365_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7365->io;
    if (swap_axes) {
        st7365->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st7365->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7365->madctl_val
    }, 1), TAG, "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_st7365_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    st7365->x_gap = x_gap;
    st7365->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7365_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7365->io;
    int command = 0;
    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_st7365_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7365_panel_t *st7365 = __containerof(panel, st7365_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7365->io;
    int command = 0;
    if (sleep) {
        command = LCD_CMD_SLPIN;
    } else {
        command = LCD_CMD_SLPOUT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}
