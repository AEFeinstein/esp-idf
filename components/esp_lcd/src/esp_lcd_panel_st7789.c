/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <stdlib.h>
#include <sys/cdefs.h>
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

static const char *TAG = "lcd_panel.st7789";

static esp_err_t panel_st7789_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_st7789_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7789_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7789_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7789_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7789_disp_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t esp_lcd_new_panel_st77xx(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, bool isST7789, esp_lcd_panel_handle_t *ret_panel);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    unsigned int bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_cal; // save surrent value of LCD_CMD_COLMOD register
} st7789_panel_t;

esp_err_t esp_lcd_new_panel_st7735(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    return esp_lcd_new_panel_st77xx(io, panel_dev_config, false, ret_panel);
}

esp_err_t esp_lcd_new_panel_st7789(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    return esp_lcd_new_panel_st77xx(io, panel_dev_config, true, ret_panel);
}

static esp_err_t esp_lcd_new_panel_st77xx(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, bool isST7789, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st7789_panel_t *st7789 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    st7789 = calloc(1, sizeof(st7789_panel_t));
    ESP_GOTO_ON_FALSE(st7789, ESP_ERR_NO_MEM, err, TAG, "no mem for st7789 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->color_space) {
    case ESP_LCD_COLOR_SPACE_RGB:
        st7789->madctl_val = 0;
        break;
    case ESP_LCD_COLOR_SPACE_BGR:
        st7789->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        st7789->colmod_cal = 0x55;
        break;
    case 18:
        st7789->colmod_cal = 0x66;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7789->io = io;
    st7789->bits_per_pixel = panel_dev_config->bits_per_pixel;
    st7789->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7789->reset_level = panel_dev_config->flags.reset_active_high;
    st7789->base.del = panel_st7789_del;
    st7789->base.reset = panel_st7789_reset;
    if(isST7789)
    {
        st7789->base.init = panel_st7789_init;
    }
    else
    {
        st7789->base.init = panel_st7735_init;
    }
    st7789->base.draw_bitmap = panel_st7789_draw_bitmap;
    st7789->base.invert_color = panel_st7789_invert_color;
    st7789->base.set_gap = panel_st7789_set_gap;
    st7789->base.mirror = panel_st7789_mirror;
    st7789->base.swap_xy = panel_st7789_swap_xy;
    st7789->base.disp_off = panel_st7789_disp_off;
    *ret_panel = &(st7789->base);
    ESP_LOGD(TAG, "new st7789 panel @%p", st7789);

    return ESP_OK;

err:
    if (st7789) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7789);
    }
    return ret;
}

static esp_err_t panel_st7789_del(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);

    if (st7789->reset_gpio_num >= 0) {
        gpio_reset_pin(st7789->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7789 panel @%p", st7789);
    free(st7789);
    return ESP_OK;
}

static esp_err_t panel_st7789_reset(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;

    // perform hardware reset
    if (st7789->reset_gpio_num >= 0) {
        gpio_set_level(st7789->reset_gpio_num, st7789->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7789->reset_gpio_num, !st7789->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5m before sending new command
    }

    return ESP_OK;
}

static esp_err_t panel_st7789_init(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789->madctl_val,
    }, 1);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        st7789->colmod_cal,
    }, 1);
    // turn on display
    esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0);

    return ESP_OK;
}

static esp_err_t panel_st7735_init(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]){0x23}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC1, (uint8_t[]){0x10}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC5, (uint8_t[]){0x3E, 0x28}, 2);
    esp_lcd_panel_io_tx_param(io, 0xC7, (uint8_t[]){0x86}, 1);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){st7789->madctl_val}, 1);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]){st7789->colmod_cal}, 1);
    esp_lcd_panel_io_tx_param(io, 0x20, NULL, 0);
    esp_lcd_panel_io_tx_param(io, 0xB1, (uint8_t[]){0x00, 0x18}, 2);
    esp_lcd_panel_io_tx_param(io, 0xB6, (uint8_t[]){0x08, 0xA2, 0x27, 0x00}, 4);
    esp_lcd_panel_io_tx_param(io, 0x26, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t[]){0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15);
    esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t[]){0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0);

    return ESP_OK;
}

static esp_err_t panel_st7789_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = st7789->io;

    x_start += st7789->x_gap;
    x_end += st7789->x_gap;
    y_start += st7789->y_gap;
    y_end += st7789->y_gap;

    // define an area of frame memory where MCU can access
    esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4);
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * st7789->bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_st7789_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}

static esp_err_t panel_st7789_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    if (mirror_x) {
        st7789->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7789->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7789->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7789->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_st7789_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    if (swap_axes) {
        st7789->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st7789->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_st7789_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    st7789->x_gap = x_gap;
    st7789->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7789_disp_off(esp_lcd_panel_t *panel, bool off)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    int command = 0;
    if (off) {
        command = LCD_CMD_DISPOFF;
    } else {
        command = LCD_CMD_DISPON;
    }
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}
