#pragma once

#include "esp_err.h"

typedef enum {
    TOUCH_INPUT_NONE = 0,
    TOUCH_INPUT_PRESS,
} touch_input_event_t;

esp_err_t touch_input_init(int gpio);
touch_input_event_t touch_input_poll(void);
