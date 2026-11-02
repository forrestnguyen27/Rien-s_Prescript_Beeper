#include "driver/gpio.h"
#include "touch_input.h"
#include "esp_timer.h"

#define DEBOUNCE_US 180000

static gpio_num_t input_pin;
static int previous_input;
static int64_t last_event_us;

esp_err_t touch_input_init(int gpio)
{
    input_pin = (gpio_num_t)gpio;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << input_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    previous_input = gpio_get_level(input_pin);
    return ESP_OK;
}

touch_input_event_t touch_input_poll(void)
{
    int input = gpio_get_level(input_pin);
    int input_rise = input && !previous_input;
    previous_input = input;

    int64_t now = esp_timer_get_time();
    if (now - last_event_us < DEBOUNCE_US) return TOUCH_INPUT_NONE;
    if (input_rise) {
        last_event_us = now;
        return TOUCH_INPUT_PRESS;
    }
    return TOUCH_INPUT_NONE;
}
