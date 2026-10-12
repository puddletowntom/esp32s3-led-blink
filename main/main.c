#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "led_strip.h"
#include "wifi_secrets.h"

/* Onboard WS2812 addressable RGB LED of ESP32-S3-DevKitC-1: GPIO48
 *
 * Firmware v1 ("wifi-ota-test v1"):
 *   - WiFi disconnected : LED double-blinks RED with a 1s dark pause
 *   - WiFi connected    : LED solid GREEN
 * The partition table carries two OTA slots (ota_0/ota_1) so firmware v2
 * (blue-disconnected variant) can be delivered over the air later.
 */
#define LED_PIN       GPIO_NUM_48
#define LED_COUNT     1
#define BLINK_ON_MS   500
#define BLINK_GAP_MS  200
#define PAUSE_MS      1000

#define RED   255, 0, 0
#define GREEN 0, 255, 0
#define BLUE  0, 0, 255

/* Disconnect-indicator color: red in the v1 build, blue in the v2 build
 * (CONFIG_OTA_V2_BLUE set via sdkconfig.v2.defaults). */
#ifdef CONFIG_OTA_V2_BLUE
#define DISC_COLOR  BLUE
#define DISC_NAME   "DISCONNECTED_BLUE"
#else
#define DISC_COLOR  RED
#define DISC_NAME   "DISCONNECTED_RED"
#endif

/* Where the app checks for OTA updates (HTTP only for LAN testing;
 * CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y must stay a conscious choice). */
#define OTA_URL "http://192.168.0.201:8000/firmware_v2.bin"

static const char *TAG = "app";

static led_strip_handle_t led;

static void set_color(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}

static void blink_twice(uint8_t r, uint8_t g, uint8_t b, const char *name) {
    for (int i = 0; i < 2; i++) {
        set_color(r, g, b);
        printf("%s\n", name);
        vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
        led_strip_clear(led);
        vTaskDelay(pdMS_TO_TICKS(BLINK_GAP_MS));
    }
}

static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0

/* --- OTA --------------------------------------------------------------- */

/* Compare dotted semver strings: returns >0 if b is newer than a. */
static int semver_newer(const char *a, const char *b) {
    int va[3] = {0}, vb[3] = {0};
    if (sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]) != 3) return -1;
    if (sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]) != 3) return -1;
    for (int i = 0; i < 3; i++) {
        if (vb[i] != va[i]) return vb[i] - va[i];
    }
    return 0;
}

static void ota_task(void *arg) {
    const esp_app_desc_t *cur = esp_app_get_description();
    ESP_LOGI(TAG, "OTA check: running v%s, fetching %s", cur->version, OTA_URL);

    esp_http_client_config_t http_cfg = {
        .url = OTA_URL,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, -1, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    const esp_app_desc_t *new_desc = esp_https_ota_get_img_desc(handle);
    if (new_desc == NULL) {
        ESP_LOGE(TAG, "OTA: could not read image descriptor, aborting");
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "OTA: server offers v%s", new_desc->version);

    if (semver_newer(cur->version, new_desc->version) <= 0) {
        ESP_LOGI(TAG, "OTA: running v%s is up to date (offered v%s), skipping",
                 cur->version, new_desc->version);
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) { }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t set_err = esp_ota_set_boot_partition(
        esp_ota_get_next_update_partition(NULL));
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: could not set boot partition: %s",
                 esp_err_to_name(set_err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "OTA to v%s complete, rebooting...", new_desc->version);
    esp_restart();
}

static void maybe_start_ota(void) {
    static bool ota_started = false;
    if (ota_started) return;
    ota_started = true;
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
}

/* ----------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "wifi sta start, connecting...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "wifi disconnected, retrying...");
            esp_wifi_connect();
            xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "GOT_IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        /* With rollback enabled, a freshly-OTA'd image boots in
         * PENDING_VERIFY; connecting to WiFi is our self-test. */
        esp_ota_img_states_t st;
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "new image verified, marking valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id, got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "%s v%s", desc->project_name, desc->version);

    /* NVS is required by the WiFi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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

    wifi_init();

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(PAUSE_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            set_color(GREEN);
            printf("CONNECTED\n");
            vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
            maybe_start_ota();  /* one OTA check per boot, right after first connect */
        } else {
            blink_twice(DISC_COLOR, DISC_NAME);
            printf("PAUSE\n");
        }
    }
}
