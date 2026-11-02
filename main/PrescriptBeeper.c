#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "buzzer.h"
#include "ssd1306.h"
#include "touch_input.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* Change these GPIOs to match your ESP32 board. */
#define OLED_SDA_GPIO 21
#define OLED_SCL_GPIO 22
#define TOUCH_INPUT_GPIO 18
#define BUZZER_GPIO 23
#define OLED_I2C_ADDRESS 0x3C

#define WIFI_CONNECTED_BIT BIT0
#define UI_REMINDER_MS (10 * 60 * 1000)
#define PROMPT_MAX_LEN 96
#define HTTP_RESPONSE_MAX 4096

static const char *TAG = "prescript_beeper";
static const char *const fallback_prompts[] = {
    "Sneak 'This is this and that is that' into a sentence.",
    "Kill the target using only a spork. The deadline is exactly midnight.",
    "An assassination will occur before your eyes. Do nothing but watch it happen.",
    "Before your life ends, recite: 'O Pain, you are my only one.' Do not weep.",
    "Buy three ripe apples. Eat only the skins. Throw the rest into the river.",
    "Stand on one foot for exactly seven minutes while watching the sunrise.",
    "Do not speak to anyone wearing blue today. Respond only with a nod.",
    "Wear your coat inside out until the clock strikes three in the afternoon.",
    "Pour your hot tea onto the floor. Drink the lukewarm water from the tap.",
    "Walk 100 paces backward before entering your home. Do not look behind you.",
    "Crack yo hb twin.",
};

typedef enum {
    UI_EVENT_INPUT,
    UI_EVENT_PROMPT_READY,
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    uint32_t request_id;
    char text[PROMPT_MAX_LEN];
} ui_event_t;

typedef struct {
    uint32_t request_id;
    char fallback[PROMPT_MAX_LEN];
} prompt_request_t;

typedef struct {
    char data[HTTP_RESPONSE_MAX];
    size_t length;
    bool overflow;
} http_response_t;

static QueueHandle_t ui_queue;
static QueueHandle_t prompt_queue;
static EventGroupHandle_t wifi_events;
static uint32_t active_request_id;
static char active_prompt[PROMPT_MAX_LEN];
static bool prompt_visible;
static bool prompt_waiting;
static char waiting_glyphs[8][21];

static char random_glyph(char correct, char previous)
{
    static const char glyphs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,!?-";
    char candidate;
    do {
        candidate = glyphs[esp_random() % (sizeof(glyphs) - 1)];
    } while (candidate == correct || candidate == previous);
    return candidate;
}

static void begin_waiting_scramble(void)
{
    ssd1306_clear();
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 21; ++col) {
            waiting_glyphs[row][col] = random_glyph(' ', '\0');
            ssd1306_draw_char_cell(col * 6, row * 8, waiting_glyphs[row][col]);
            buzzer_switch_tick();
        }
    }
    ESP_ERROR_CHECK(ssd1306_flush());
}

static void scramble_waiting_frame(void)
{
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 21; ++col) {
            waiting_glyphs[row][col] = random_glyph(' ', waiting_glyphs[row][col]);
            ssd1306_draw_char_cell(col * 6, row * 8, waiting_glyphs[row][col]);
            buzzer_switch_tick();
        }
    }
    ESP_ERROR_CHECK(ssd1306_flush());
}

static void animate_prompt(const char *prompt)
{
    int64_t started_us = esp_timer_get_time();
    enum { SCRAMBLE_ROWS = 8, SCRAMBLE_COLS = 21, SCRAMBLE_FRAMES = 8 };
    char target[SCRAMBLE_ROWS][SCRAMBLE_COLS] = {{0}};
    char shown[SCRAMBLE_ROWS][SCRAMBLE_COLS] = {{0}};
    bool has_target[SCRAMBLE_ROWS][SCRAMBLE_COLS] = {{false}};
    bool fixed[SCRAMBLE_ROWS][SCRAMBLE_COLS] = {{false}};
    size_t target_count = 0;

    int row = 0, col = 0;
    const char *cursor = prompt;
    while (*cursor && row < SCRAMBLE_ROWS) {
        while (*cursor == ' ') ++cursor;
        if (!*cursor) break;

        const char *word = cursor;
        size_t word_length = 0;
        while (cursor[word_length] && cursor[word_length] != ' ' &&
               cursor[word_length] != '\n')
            ++word_length;

        if (col > 0 && col + 1 + (int)word_length > SCRAMBLE_COLS) {
            ++row;
            col = 0;
        } else if (col > 0) {
            target[row][col] = ' ';
            has_target[row][col++] = true;
            ++target_count;
        }

        for (size_t i = 0; i < word_length && row < SCRAMBLE_ROWS; ++i) {
            if (col >= SCRAMBLE_COLS) {
                ++row;
                col = 0;
                if (row >= SCRAMBLE_ROWS) break;
            }
            target[row][col] = word[i];
            has_target[row][col++] = true;
            ++target_count;
        }
        cursor += word_length;
        if (*cursor == '\n') {
            ++row;
            col = 0;
            ++cursor;
        }
    }

    ssd1306_clear();
    /* Start with the whole panel scrambled, including the unused area. */
    for (int row = 0; row < SCRAMBLE_ROWS; ++row) {
        for (int col = 0; col < SCRAMBLE_COLS; ++col) {
            shown[row][col] = random_glyph(' ', '\0');
            ssd1306_draw_char_cell(col * 6, row * 8, shown[row][col]);
            buzzer_switch_tick();
        }
    }
    ESP_ERROR_CHECK(ssd1306_flush());

    size_t revealed = 0;
    for (int frame = 0; frame < SCRAMBLE_FRAMES; ++frame) {
        size_t reveal_goal = (target_count * (size_t)(frame + 1) + SCRAMBLE_FRAMES - 1) /
                             SCRAMBLE_FRAMES;
        for (int row = 0; row < SCRAMBLE_ROWS; ++row) {
            for (int col = 0; col < SCRAMBLE_COLS; ++col) {
                if (fixed[row][col]) continue;
                if (has_target[row][col] && revealed < reveal_goal) {
                    shown[row][col] = target[row][col];
                    fixed[row][col] = true;
                    ++revealed;
                } else if (!has_target[row][col] &&
                           (frame == SCRAMBLE_FRAMES - 1 || (esp_random() & 3u) == 0)) {
                    /* Unused cells wink out at random while the prompt resolves. */
                    shown[row][col] = ' ';
                    fixed[row][col] = true;
                } else {
                    shown[row][col] = random_glyph(
                        has_target[row][col] ? target[row][col] : ' ', shown[row][col]);
                }
                ssd1306_draw_char_cell(col * 6, row * 8, shown[row][col]);
                buzzer_switch_tick();
            }
        }
        ESP_ERROR_CHECK(ssd1306_flush());
    }
    ESP_LOGI(TAG, "Prompt reveal took %lld ms",
             (long long)((esp_timer_get_time() - started_us) / 1000));
}

static void draw_prompt_static(const char *prompt)
{
    ssd1306_clear();
    int x = 0, y = 0;
    for (const char *p = prompt; *p && y < 64; ++p) {
        if (*p == '\n') { x = 0; y += 8; continue; }
        if (x >= 126) { x = 0; y += 8; }
        if (y < 64) {
            char one[2] = {*p, '\0'};
            ssd1306_draw_text(x, y, one);
        }
        x += 6;
    }
    ESP_ERROR_CHECK(ssd1306_flush());
}

static void draw_clear_message(void)
{
    const char *message = "._CLEAR";
    const int scale = 2;
    const int width = (int)strlen(message) * 6 * scale;
    ssd1306_clear();
    ssd1306_draw_text_scaled((128 - width) / 2, (64 - 7 * scale) / 2, message, scale);
    ESP_ERROR_CHECK(ssd1306_flush());
}

static bool deepseek_available(void)
{
    return wifi_events != NULL && CONFIG_PRESCRIPT_DEEPSEEK_API_KEY[0] != '\0' &&
           (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

static void sanitize_prompt(char output[PROMPT_MAX_LEN], const char *input)
{
    size_t n = 0;
    while (*input && n + 1 < PROMPT_MAX_LEN) {
        unsigned char c = (unsigned char)*input++;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == ',' ||
              c == '!' || c == '?' || c == '\'' || c == '-')) c = ' ';
        if (c != ' ' || (n > 0 && output[n - 1] != ' ')) output[n++] = (char)c;
    }
    while (n > 0 && output[n - 1] == ' ') --n;
    output[n] = '\0';
}

static void play_beeps(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        buzzer_tone(1500, 75);
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void wifi_start(void)
{
    wifi_events = xEventGroupCreate();
    if (wifi_events == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi event group");
        return;
    }
    if (CONFIG_PRESCRIPT_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty; using built-in prompts");
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", CONFIG_PRESCRIPT_WIFI_SSID);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", CONFIG_PRESCRIPT_WIFI_PASSWORD);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t collect_http_response(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ERROR) {
        ESP_LOGW(TAG, "HTTP client reported a transport error");
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && response && event->data_len > 0) {
        size_t available = sizeof(response->data) - response->length - 1;
        size_t amount = (size_t)event->data_len < available ? (size_t)event->data_len : available;
        memcpy(response->data + response->length, event->data, amount);
        response->length += amount;
        if (amount < (size_t)event->data_len) response->overflow = true;
        response->data[response->length] = '\0';
    }
    return ESP_OK;
}

/* Extract and unescape a JSON string value for the named property. */
static bool json_string_field(const char *json, const char *name,
                              char *output, size_t output_size)
{
    char key[40];
    if (snprintf(key, sizeof(key), "\"%s\"", name) >= (int)sizeof(key)) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != '"') return false;

    size_t n = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (!c) return false;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': case 'f': case 'n': case 'r': case 't': c = ' '; break;
            case 'u':
                for (int i = 0; i < 4 && *p; ++i) ++p;
                c = ' ';
                break;
            default: return false;
            }
        }
        if (n + 1 < output_size) output[n++] = (char)c;
    }
    if (*p != '"') return false;
    output[n] = '\0';
    return n > 0;
}

static bool deepseek_prompt(char out[PROMPT_MAX_LEN])
{
    if (!wifi_events || CONFIG_PRESCRIPT_DEEPSEEK_API_KEY[0] == '\0' ||
        !(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "DeepSeek skipped: Wi-Fi or API key is unavailable");
        return false;
    }

    char body[512];
    int body_length = snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"max_tokens\":96,\"temperature\":0.9,\"thinking\":{\"type\":\"disabled\"},\"messages\":["
        "{\"role\":\"system\",\"content\":\"Write one short, obscure prescript for an index proxy from Project Moon. Return only the prompt, at most 90 characters.\"},"
        "{\"role\":\"user\",\"content\":\"Give me a fresh prescript.\"}]}",
        CONFIG_PRESCRIPT_DEEPSEEK_MODEL);
    if (body_length < 0 || (size_t)body_length >= sizeof(body)) return false;
    http_response_t response = {0};
    esp_http_client_config_t http_config = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = collect_http_response,
        .user_data = &response,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) return false;
    char authorization[sizeof(CONFIG_PRESCRIPT_DEEPSEEK_API_KEY) + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_PRESCRIPT_DEEPSEEK_API_KEY);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, body_length);
    ESP_LOGI(TAG, "Sending DeepSeek prompt request");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "DeepSeek HTTP status=%d, response bytes=%u%s", status,
             (unsigned)response.length, response.overflow ? " (truncated)" : "");
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "DeepSeek request failed (err=%s, status=%d)", esp_err_to_name(err), status);
        return false;
    }

    if (response.overflow || !json_string_field(response.data, "content", out, PROMPT_MAX_LEN)) {
        ESP_LOGW(TAG, "DeepSeek response did not contain a complete content field");
        return false;
    }
    sanitize_prompt(out, out);
    if (out[0] == '\0') {
        ESP_LOGW(TAG, "DeepSeek returned an empty prompt");
        return false;
    }
    ESP_LOGI(TAG, "DeepSeek prompt received: %s", out);
    return true;
}

static void touch_task(void *arg)
{
    (void)arg;
    while (true) {
        touch_input_event_t input = touch_input_poll();
        if (input != TOUCH_INPUT_NONE) {
            ESP_LOGI(TAG, "Touch detected on prompt sensor");
            ui_event_t event = {.type = UI_EVENT_INPUT};
            if (xQueueSend(ui_queue, &event, 0) != pdTRUE)
                ESP_LOGW(TAG, "UI queue full; touch event dropped");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void prompt_task(void *arg)
{
    (void)arg;
    prompt_request_t request;
    while (xQueueReceive(prompt_queue, &request, portMAX_DELAY) == pdTRUE) {
        char result[PROMPT_MAX_LEN];
        if (!deepseek_prompt(result)) {
            snprintf(result, sizeof(result), "%s", request.fallback);
            ESP_LOGI(TAG, "Keeping built-in fallback prompt");
        }
        ui_event_t event = {.type = UI_EVENT_PROMPT_READY, .request_id = request.request_id};
        snprintf(event.text, sizeof(event.text), "%s", result);
        if (xQueueSend(ui_queue, &event, portMAX_DELAY) != pdTRUE)
            ESP_LOGE(TAG, "Could not deliver prompt result to UI task");
    }
}

static void show_startup_logo(void)
{
    ssd1306_clear();
    ssd1306_draw_logo_centered();
    ESP_ERROR_CHECK(ssd1306_flush());
}

static void ui_task(void *arg)
{
    (void)arg;
    TickType_t last_reminder = xTaskGetTickCount();
    ui_event_t event;
    while (true) {
        TickType_t elapsed = xTaskGetTickCount() - last_reminder;
        TickType_t reminder_ticks = pdMS_TO_TICKS(UI_REMINDER_MS);
        TickType_t wait = elapsed >= reminder_ticks ? 0 : reminder_ticks - elapsed;
        if (prompt_waiting && wait > pdMS_TO_TICKS(20)) wait = pdMS_TO_TICKS(20);
        if (xQueueReceive(ui_queue, &event, wait) == pdTRUE) {
            if (event.type == UI_EVENT_INPUT) {
                if (!prompt_visible) {
                    const size_t count = sizeof(fallback_prompts) / sizeof(fallback_prompts[0]);
                    snprintf(active_prompt, sizeof(active_prompt), "%s", fallback_prompts[esp_random() % count]);
                    prompt_visible = true;
                    ++active_request_id;
                    play_beeps(1);
                    prompt_request_t request = {.request_id = active_request_id};
                    snprintf(request.fallback, sizeof(request.fallback), "%s", active_prompt);
                    if (deepseek_available()) {
                        ESP_LOGI(TAG, "Requesting one DeepSeek prompt");
                        if (xQueueSend(prompt_queue, &request, 0) != pdTRUE) {
                            ESP_LOGW(TAG, "Prompt queue full; displaying built-in fallback");
                            animate_prompt(active_prompt);
                        } else {
                            prompt_waiting = true;
                            begin_waiting_scramble();
                        }
                    } else {
                        ESP_LOGI(TAG, "Wi-Fi or API key unavailable; displaying one built-in prompt");
                        animate_prompt(active_prompt);
                    }
                } else {
                    prompt_visible = false;
                    prompt_waiting = false;
                    ++active_request_id;
                    draw_clear_message();
                    play_beeps(3);
                }
            } else if (event.type == UI_EVENT_PROMPT_READY && prompt_visible &&
                       event.request_id == active_request_id) {
                snprintf(active_prompt, sizeof(active_prompt), "%s", event.text);
                prompt_waiting = false;
                animate_prompt(active_prompt);
                ESP_LOGI(TAG, "Updated OLED with DeepSeek prompt");
            }
        } else {
            if (prompt_waiting) {
                scramble_waiting_frame();
                continue;
            }
            show_startup_logo();
            play_beeps(4);
            prompt_visible = false;
            ++active_request_id;
            last_reminder = xTaskGetTickCount();
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(ssd1306_init(OLED_SDA_GPIO, OLED_SCL_GPIO, OLED_I2C_ADDRESS));
    ESP_ERROR_CHECK(touch_input_init(TOUCH_INPUT_GPIO));
    ESP_ERROR_CHECK(buzzer_init(BUZZER_GPIO));
    show_startup_logo();

    ui_queue = xQueueCreate(8, sizeof(ui_event_t));
    prompt_queue = xQueueCreate(2, sizeof(prompt_request_t));
    if (!ui_queue || !prompt_queue) {
        ESP_LOGE(TAG, "Could not create application queues");
        return;
    }
    wifi_start();
    if (xTaskCreate(ui_task, "prescript_ui", 4096, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Could not create UI task");
    if (xTaskCreate(touch_task, "touch_input", 2048, NULL, 6, NULL) != pdPASS)
        ESP_LOGE(TAG, "Could not create touch task");
    if (xTaskCreate(prompt_task, "deepseek_prompt", 10240, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "Could not create prompt task");
}
