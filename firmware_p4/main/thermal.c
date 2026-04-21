// Thermal task: pulls frames from the MLX90640 in the background and
// publishes them behind a mutex for the HTTP handler to copy out.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "app_config.h"
#include "thermal.h"
#include "mlx90640_driver.h"

static const char *TAG = "thermal";

static mlx90640_t       s_mlx;
static bool             s_ok = false;
static SemaphoreHandle_t s_mux;
static float            s_frame[THERMAL_W * THERMAL_H];
static float            s_tmin = 0, s_tmax = 0;
static uint32_t         s_seq = 0;

static void thermal_task(void *arg) {
    float local[THERMAL_W * THERMAL_H];
    while (1) {
        float mn, mx;
        if (mlx90640_read_frame(&s_mlx, local, &mn, &mx) == ESP_OK) {
            if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
                memcpy(s_frame, local, sizeof(local));
                s_tmin = mn;
                s_tmax = mx;
                s_seq++;
                xSemaphoreGive(s_mux);
            }
        } else {
            ESP_LOGW(TAG, "frame read timeout");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t thermal_start(void) {
    s_mux = xSemaphoreCreateMutex();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = MLX_I2C_PORT,
        .sda_io_num = MLX_I2C_SDA_GPIO,
        .scl_io_num = MLX_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  // we rely on the breakout's
                                                // 4.7k external pull-ups
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mlx90640_init(&s_mlx, bus, MLX_I2C_ADDR, MLX_I2C_HZ);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mlx90640 not detected on SDA=%d SCL=%d: %s",
                 MLX_I2C_SDA_GPIO, MLX_I2C_SCL_GPIO, esp_err_to_name(err));
        return err;
    }

    mlx90640_set_refresh_hz(&s_mlx, MLX_REFRESH_HZ);
    s_ok = true;

    xTaskCreate(thermal_task, "thermal", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "thermal up at %d Hz", MLX_REFRESH_HZ);
    return ESP_OK;
}

bool thermal_available(void) { return s_ok; }

esp_err_t thermal_snapshot(float *dst, float *tmin, float *tmax, uint32_t *seq,
                           uint32_t timeout_ms) {
    if (!s_ok) return ESP_ERR_NOT_FOUND;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_seq == 0) {
        xSemaphoreGive(s_mux);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(dst, s_frame, sizeof(s_frame));
    if (tmin) *tmin = s_tmin;
    if (tmax) *tmax = s_tmax;
    if (seq)  *seq  = s_seq;
    xSemaphoreGive(s_mux);
    return ESP_OK;
}
