idf_component_register(
    SRCS "../ssd1306.c" "../touch_input.c" "../buzzer.c"
    INCLUDE_DIRS ".."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_timer esp_rom
)
