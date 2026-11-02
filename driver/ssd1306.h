#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t ssd1306_init(int sda_gpio, int scl_gpio, unsigned char address);
void ssd1306_clear(void);
void ssd1306_draw_text(int x, int y, const char *text);
void ssd1306_draw_bitmap(int x, int y, unsigned width, unsigned height,
                         const uint8_t *bitmap);
void ssd1306_draw_char_cell(int x, int y, char character);
esp_err_t ssd1306_flush_cell(int x, int y);
void ssd1306_draw_text_scaled(int x, int y, const char *text, unsigned scale);
void ssd1306_draw_logo_centered(void);
esp_err_t ssd1306_flush(void);
