// MIPI-CSI capture (OV5647) + P4 hardware JPEG encode.
//
// This follows the structure of the esp-idf
// `examples/peripherals/camera/mipi_isp_dsi` reference project, which is the
// closest thing to an "official" P4 camera example. The APIs are still
// evolving between IDF 5.3 / 5.4 / 5.5 — if a struct field below doesn't
// exist in your exact IDF checkout, open that example and copy the current
// member names. The overall flow is stable:
//
//   init ISP  ->  esp_cam_new_csi_ctlr  ->  register buffers
//   esp_cam_ctlr_start  ->  loop:
//       esp_cam_ctlr_receive(&trans)      // blocks for a filled buffer
//       jpeg_encoder_process(...)         // hardware JPEG compress
//       publish pointer to HTTP layer
//       esp_cam_ctlr_receive again to recycle
//
// The sensor driver (OV5647) is auto-detected by `esp_cam_sensor` over the
// camera's private SCCB bus, which is separate from our user I2C.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_sccb_intf.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/jpeg_encode.h"

#include "app_config.h"
#include "camera.h"

static const char *TAG = "cam";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  *raw;       // RGB565 or YUV422 buffer from the camera
    size_t    raw_len;
    uint8_t  *jpg;       // JPEG buffer (we swap between two of these)
    size_t    jpg_cap;
    size_t    jpg_len;
} frame_slot_t;

#define JPG_MAX_BYTES  (CAM_WIDTH * CAM_HEIGHT / 4)  // generous 4:1 floor

static esp_cam_ctlr_handle_t   s_cam_ctlr;
static jpeg_encoder_handle_t   s_jpg;
static frame_slot_t            s_slot_a, s_slot_b;
static frame_slot_t * volatile s_latest = NULL;   // most recent JPEG
static SemaphoreHandle_t       s_publish_mux;
static SemaphoreHandle_t       s_reader_mux;     // serialises HTTP borrows
static SemaphoreHandle_t       s_frame_evt;      // signalled when a new JPEG is ready

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void *psram_alloc(size_t n) {
    return heap_caps_aligned_alloc(64, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t init_sensor_i2c(i2c_master_bus_handle_t *bus_out,
                                 esp_sccb_io_handle_t *sccb_out) {
    // Camera SCCB — the dedicated sensor I2C, NOT the user I2C on GPIO7/8.
    // The Nano routes SCCB to GPIO34/35 per the schematic; adjust if your
    // board rev differs.
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = 34,
        .scl_io_num = 35,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, bus_out));
    sccb_i2c_config_t sccb_cfg = {
        .scl_speed_hz = 100000,
        .device_address = 0x36,          // OV5647 7-bit addr
        .addr_bits_width = 16,
        .val_bits_width  = 8,
    };
    return sccb_new_i2c_io(*bus_out, &sccb_cfg, sccb_out);
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

static void capture_task(void *arg) {
    // Ping-pong between the two slots so the HTTP layer can be reading A
    // while capture is refilling B.
    frame_slot_t *slots[2] = { &s_slot_a, &s_slot_b };
    int idx = 0;

    while (1) {
        frame_slot_t *s = slots[idx];
        esp_cam_ctlr_trans_t tr = {
            .buffer = s->raw,
            .buflen = s->raw_len,
        };
        if (esp_cam_ctlr_receive(s_cam_ctlr, &tr, pdMS_TO_TICKS(1000)) != ESP_OK) {
            ESP_LOGW(TAG, "frame receive timeout");
            continue;
        }

        jpeg_encode_cfg_t enc = {
            .src_type     = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample   = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = CAM_JPEG_Q,
            .width        = CAM_WIDTH,
            .height       = CAM_HEIGHT,
        };
        uint32_t out_size = 0;
        esp_err_t err = jpeg_encoder_process(
            s_jpg, &enc, s->raw, s->raw_len,
            s->jpg, s->jpg_cap, &out_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "jpeg encode failed: %s", esp_err_to_name(err));
            continue;
        }
        s->jpg_len = out_size;

        xSemaphoreTake(s_publish_mux, portMAX_DELAY);
        s_latest = s;
        xSemaphoreGive(s_publish_mux);
        xSemaphoreGive(s_frame_evt);

        idx ^= 1;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t camera_start(void) {
    s_publish_mux = xSemaphoreCreateMutex();
    s_reader_mux  = xSemaphoreCreateMutex();
    s_frame_evt   = xSemaphoreCreateBinary();

    // Allocate raw + jpeg buffers in PSRAM.
    size_t raw_len = CAM_WIDTH * CAM_HEIGHT * 2;  // RGB565 bytes/pixel
    s_slot_a.raw = psram_alloc(raw_len);
    s_slot_b.raw = psram_alloc(raw_len);
    s_slot_a.jpg = psram_alloc(JPG_MAX_BYTES);
    s_slot_b.jpg = psram_alloc(JPG_MAX_BYTES);
    if (!s_slot_a.raw || !s_slot_b.raw || !s_slot_a.jpg || !s_slot_b.jpg) {
        ESP_LOGE(TAG, "out of PSRAM for camera buffers");
        return ESP_ERR_NO_MEM;
    }
    s_slot_a.raw_len = s_slot_b.raw_len = raw_len;
    s_slot_a.jpg_cap = s_slot_b.jpg_cap = JPG_MAX_BYTES;

    // SCCB + sensor probe.
    i2c_master_bus_handle_t i2c_bus;
    esp_sccb_io_handle_t    sccb;
    ESP_ERROR_CHECK(init_sensor_i2c(&i2c_bus, &sccb));

    esp_cam_sensor_config_t sensor_cfg = {
        .sccb_handle = sccb,
        .reset_pin   = -1,    // tied high on the Nano; driver pulses via XSHUTDOWN not needed
        .pwdn_pin    = -1,
        .xclk_pin    = -1,    // P4 ISP generates XCLK internally for MIPI sensors
        .xclk_freq_hz = 24000000,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };
    esp_cam_sensor_device_t *sensor = esp_cam_sensor_detect(&sensor_cfg);
    if (!sensor) {
        ESP_LOGE(TAG, "no MIPI sensor detected on SCCB");
        return ESP_FAIL;
    }
    // Pick a format that matches our CAM_WIDTH/CAM_HEIGHT. The registry
    // of formats is sensor-specific; `esp_cam_sensor_query_format_array`
    // lets you enumerate. For OV5647 at 720p we ask for RGB565 by name.
    esp_cam_sensor_format_t fmt = { 0 };
    esp_cam_sensor_format_array_t arr = { 0 };
    esp_cam_sensor_query_format_array(sensor, &arr);
    for (int i = 0; i < arr.count; ++i) {
        if (arr.format_array[i].width == CAM_WIDTH &&
            arr.format_array[i].height == CAM_HEIGHT) {
            fmt = arr.format_array[i];
            break;
        }
    }
    if (fmt.width == 0) {
        ESP_LOGW(TAG, "no exact %dx%d format; falling back to index 0", CAM_WIDTH, CAM_HEIGHT);
        fmt = arr.format_array[0];
    }
    ESP_ERROR_CHECK(esp_cam_sensor_set_format(sensor, &fmt));

    // CSI receiver.
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id     = 0,
        .clk_src     = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res       = CAM_WIDTH,
        .v_res       = CAM_HEIGHT,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,   // typical OV5647 output
        .output_data_color_type = CAM_CTLR_COLOR_RGB565, // ISP converts
        .data_lane_num = 2,
        .byte_swap_en  = false,
        .queue_items   = CAM_FB_COUNT,
    };
    ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam_ctlr));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(s_cam_ctlr));
    ESP_ERROR_CHECK(esp_cam_ctlr_start(s_cam_ctlr));

    // Hardware JPEG encoder.
    jpeg_encode_engine_cfg_t jpg_cfg = {
        .timeout_ms = 100,
    };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jpg_cfg, &s_jpg));

    xTaskCreatePinnedToCore(capture_task, "cam_cap", 8192, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "camera up: %dx%d q=%d", CAM_WIDTH, CAM_HEIGHT, CAM_JPEG_Q);
    return ESP_OK;
}

esp_err_t camera_get_jpeg(const uint8_t **buf, size_t *len, uint32_t timeout_ms) {
    if (xSemaphoreTake(s_reader_mux, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    // Wait for at least one published frame.
    if (xSemaphoreTake(s_frame_evt, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        xSemaphoreGive(s_reader_mux);
        return ESP_ERR_TIMEOUT;
    }
    frame_slot_t *s = NULL;
    xSemaphoreTake(s_publish_mux, portMAX_DELAY);
    s = (frame_slot_t *)s_latest;
    xSemaphoreGive(s_publish_mux);

    if (!s) {
        xSemaphoreGive(s_reader_mux);
        return ESP_FAIL;
    }
    *buf = s->jpg;
    *len = s->jpg_len;
    return ESP_OK;
}

void camera_release(void) {
    xSemaphoreGive(s_reader_mux);
}
