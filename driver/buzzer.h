#pragma once

#include "esp_err.h"

esp_err_t buzzer_init(int gpio);
void buzzer_tone(unsigned int frequency_hz, unsigned int duration_ms);
void buzzer_switch_tick(void);
