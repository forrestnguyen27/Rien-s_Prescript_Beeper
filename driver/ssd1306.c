#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "index_logo_64.h"
#include "ssd1306.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64

static uint8_t framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];
static i2c_master_bus_handle_t oled_bus;
static i2c_master_dev_handle_t oled_device;

/* 5x7 glyphs. Keep lowercase distinct for generated prompts. */
static const uint8_t upper_font[][5] = {
    {0,0,0,0,0}, {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41},
    {0x7f,0x09,0x09,0x09,0x01}, {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0,0x41,0x7f,0x41,0}, {0x20,0x40,0x41,0x3f,0x01}, {0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40}, {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e},
    {0x7f,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    {0x3e,0x51,0x49,0x45,0x3e}, {0,0x42,0x7f,0x40,0}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1e}, {0x08,0x08,0x08,0x08,0x08}, {0x08,0x08,0x2a,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00}, {0,0x50,0x30,0,0}, {0,0,0x5f,0,0},
    {0x02,0x01,0x51,0x09,0x06}, {0,0x05,0x03,0,0},
};
static const uint8_t lower_font[26][5] = {
    {0,0x20,0x54,0x54,0x78}, {0x7f,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7e,0x09,0x01,0x02},
    {0x0c,0x52,0x52,0x52,0x3e}, {0x7f,0x08,0x04,0x04,0x78}, {0,0x44,0x7d,0x40,0},
    {0x20,0x40,0x44,0x3d,0}, {0x7f,0x10,0x28,0x44,0}, {0,0x41,0x7f,0x40,0},
    {0x7c,0x04,0x18,0x04,0x78}, {0x7c,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7c}, {0x7c,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20}, {0x04,0x3f,0x44,0x40,0x20}, {0x3c,0x40,0x40,0x20,0x7c},
    {0x1c,0x20,0x40,0x20,0x1c}, {0x3c,0x40,0x30,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44},
    {0x0c,0x50,0x50,0x50,0x3c}, {0x44,0x64,0x54,0x4c,0x44},
};

static esp_err_t write_command(uint8_t command)
{
    const uint8_t packet[] = {0x00, command};
    return i2c_master_transmit(oled_device, packet, sizeof(packet), 100);
}

esp_err_t ssd1306_init(int sda_gpio, int scl_gpio, unsigned char address)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)sda_gpio,
        .scl_io_num = (gpio_num_t)scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &oled_bus);
    if (err != ESP_OK) return err;

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(oled_bus, &device_config, &oled_device);
    if (err != ESP_OK) return err;

    const uint8_t init_commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, /* 64-row multiplex */
        0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00,
        0xA0, 0xC0, 0xDA, 0x12, 0x81, 0x7F, 0xD9, 0xF1,
        0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
    for (size_t i = 0; i < sizeof(init_commands); ++i) {
        err = write_command(init_commands[i]);
        if (err != ESP_OK) return err;
    }
    ssd1306_clear();
    return ssd1306_flush();
}

void ssd1306_clear(void)
{
    memset(framebuffer, 0, sizeof(framebuffer));
}

static void set_pixel(int x, int y, int scale)
{
    for (int dx = 0; dx < scale; ++dx) for (int dy = 0; dy < scale; ++dy) {
        int px = x + dx, py = y + dy;
        if (px >= 0 && px < OLED_WIDTH && py >= 0 && py < OLED_HEIGHT)
            framebuffer[px + (py / 8) * OLED_WIDTH] |= (uint8_t)(1u << (py & 7));
    }
}

static void draw_char(int x, int y, char c, int scale)
{
    const uint8_t *glyph = NULL;
    if (c >= 'A' && c <= 'Z') glyph = upper_font[c - 'A' + 1];
    else if (c >= 'a' && c <= 'z') glyph = lower_font[c - 'a'];
    else if (c >= '0' && c <= '9') glyph = upper_font[c - '0' + 27];
    else if (c == '-') glyph = upper_font[37];
    else if (c == '+') glyph = upper_font[38];
    else if (c == '.') glyph = upper_font[39];
    else if (c == ',') glyph = upper_font[40];
    else if (c == '!') glyph = upper_font[41];
    else if (c == '?') glyph = upper_font[42];
    else if (c == '\'') glyph = upper_font[43];
    else if (c == '_') glyph = (const uint8_t[]){0x40,0x40,0x40,0x40,0x40};
    if (!glyph) return;
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((glyph[col] >> row) & 1) {
                for (int dx = 0; dx < scale; ++dx)
                    for (int dy = 0; dy < scale; ++dy)
                        set_pixel(x + col * scale + dx, y + row * scale + dy, 1);
            }
        }
    }
}

void ssd1306_draw_char_cell(int x, int y, char character)
{
    /* Animated text uses 6x8 cells aligned to an SSD1306 page. */
    for (int px = x; px < x + 6; ++px) {
        if (px < 0 || px >= OLED_WIDTH) continue;
        for (int py = y; py < y + 8; ++py) {
            if (py < 0 || py >= OLED_HEIGHT) continue;
            size_t index = (size_t)px + (size_t)(py / 8) * OLED_WIDTH;
            framebuffer[index] &= (uint8_t)~(1u << (py & 7));
        }
    }
    draw_char(x, y, character, 1);
}

esp_err_t ssd1306_flush_cell(int x, int y)
{
    if (x < 0 || x + 5 >= OLED_WIDTH || y < 0 || y + 7 >= OLED_HEIGHT || (y & 7) != 0)
        return ESP_ERR_INVALID_ARG;
    uint8_t commands[] = {0x00, 0x21, (uint8_t)x, (uint8_t)(x + 5),
                          0x22, (uint8_t)(y / 8), (uint8_t)(y / 8)};
    esp_err_t err = i2c_master_transmit(oled_device, commands, sizeof(commands), 100);
    if (err != ESP_OK) return err;
    uint8_t data[] = {0x40, 0, 0, 0, 0, 0, 0};
    memcpy(data + 1, framebuffer + x + (y / 8) * OLED_WIDTH, 6);
    return i2c_master_transmit(oled_device, data, sizeof(data), 100);
}

void ssd1306_draw_text_scaled(int x, int y, const char *text, unsigned scale)
{
    if (scale == 0) return;
    while (*text && x < OLED_WIDTH) {
        draw_char(x, y, *text++, (int)scale);
        x += 6 * (int)scale;
    }
}

void ssd1306_draw_text(int x, int y, const char *text)
{
    ssd1306_draw_text_scaled(x, y, text, 1);
}

void ssd1306_draw_bitmap(int x, int y, unsigned width, unsigned height,
                         const uint8_t *bitmap)
{
    if (!bitmap) return;
    unsigned row_bytes = (width + 7) / 8;
    for (unsigned row = 0; row < height; ++row) {
        for (unsigned col = 0; col < width; ++col) {
            if (bitmap[row * row_bytes + col / 8] & (1u << (col & 7)))
                set_pixel(x + (int)col, y + (int)row, 1);
        }
    }
}

void ssd1306_draw_logo_centered(void)
{
    ssd1306_draw_bitmap((OLED_WIDTH - 64) / 2, (OLED_HEIGHT - 64) / 2,
                        64, 64, index_logo_64x64);
}

esp_err_t ssd1306_flush(void)
{
    esp_err_t err = write_command(0x21);
    if (err == ESP_OK) err = write_command(0);
    if (err == ESP_OK) err = write_command(127);
    if (err == ESP_OK) err = write_command(0x22);
    if (err == ESP_OK) err = write_command(0);
    if (err == ESP_OK) err = write_command(7);
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < sizeof(framebuffer); i += 16) {
        uint8_t packet[17];
        packet[0] = 0x40;
        memcpy(packet + 1, framebuffer + i, 16);
        err = i2c_master_transmit(oled_device, packet, sizeof(packet), 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
