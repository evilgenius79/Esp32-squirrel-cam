// Squirrel cam for the Waveshare ESP32-P4-Nano.
//
// The P4 handles the MIPI-CSI camera (Pi Cam v1.3, OV5647), a MLX90640
// thermal array on I2C0, and an HTTP server. WiFi is provided by an onboard
// ESP32-C6 over SDIO via esp_wifi_remote / esp_hosted; from the app's
// perspective the usual esp_wifi_* API still works.

#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mdns.h"

#include "app_config.h"
#include "wifi.h"
#include "camera.h"
#include "thermal.h"
#include "http_server.h"

static const char *TAG = "main";

static void start_mdns(void) {
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed (non-fatal)");
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("Squirrel cam");
    mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mdns: http://%s.local/", MDNS_HOSTNAME);
}

void app_main(void) {
    ESP_LOGI(TAG, "squirrel-cam (P4) starting");

    // NVS is required by WiFi for calibration data / PHY blobs.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Bring the peripherals up first so the HTTP handlers never see a null
    // pointer if a client races the boot.
    if (thermal_start() != ESP_OK) {
        ESP_LOGW(TAG, "thermal init failed; /thermal will return an error payload");
    }
    if (camera_start() != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed; halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // WiFi last: it's the slowest to come up and we want the local sensors
    // ready by the time a client can connect.
    wifi_start_blocking();
    start_mdns();
    http_server_start();

    ESP_LOGI(TAG, "ready. open http://%s.local/", MDNS_HOSTNAME);
}
