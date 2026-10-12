#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

/* Onboard WS2812 addressable RGB LED of ESP32-S3-DevKitC-1: GPIO48 */
#define LED_PIN      GPIO_NUM_48
#define LED_COUNT    1
#define INTERVAL_MS  500

static led_strip_handle_t led;

static void set_color(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}

void app_main(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_PIN,
        .max_leds = LED_COUNT,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led));
    led_strip_clear(led);

    bool blue = true;
    while (1) {
        if (blue) {
            set_color(0, 0, 255);
            printf("BLUE\n");
        } else {
            set_color(255, 0, 0);
            printf("RED\n");
        }
        blue = !blue;
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_MS));
    }
}
