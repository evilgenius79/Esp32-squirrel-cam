// Squirrel nest cam firmware for the Seeed XIAO ESP32S3 Sense.
//
// Serves three HTTP endpoints:
//   GET /           -> tiny status/landing page
//   GET /stream     -> multipart/x-mixed-replace MJPEG video
//   GET /snapshot   -> single JPEG frame
//   GET /thermal    -> JSON { "w":32, "h":24, "min":.., "max":.., "data":[...] }
//
// The camera uses its private SCCB bus on GPIO39/40. The MLX90640 lives on the
// user I2C bus on GPIO5 (SDA) / GPIO6 (SCL), so the two never collide.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <Adafruit_MLX90640.h>

#include "config.h"
#include "camera_pins.h"

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static Adafruit_MLX90640 mlx;
static float thermal_frame[32 * 24];
static SemaphoreHandle_t thermal_mutex = nullptr;
static volatile float thermal_min = 0.0f;
static volatile float thermal_max = 0.0f;
static volatile uint32_t thermal_seq = 0;
static bool mlx_ok = false;

static httpd_handle_t server = nullptr;

// -----------------------------------------------------------------------------
// Camera
// -----------------------------------------------------------------------------

static bool init_camera() {
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = Y2_GPIO_NUM;
    cfg.pin_d1       = Y3_GPIO_NUM;
    cfg.pin_d2       = Y4_GPIO_NUM;
    cfg.pin_d3       = Y5_GPIO_NUM;
    cfg.pin_d4       = Y6_GPIO_NUM;
    cfg.pin_d5       = Y7_GPIO_NUM;
    cfg.pin_d6       = Y8_GPIO_NUM;
    cfg.pin_d7       = Y9_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;
    cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = CAMERA_FRAMESIZE;
    cfg.jpeg_quality = JPEG_QUALITY;
    cfg.fb_count     = psramFound() ? 2 : 1;
    cfg.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        log_e("camera init failed: 0x%x", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        // Mild defaults that work well in dim nests.
        s->set_brightness(s, 1);
        s->set_saturation(s, -1);
        s->set_gainceiling(s, (gainceiling_t)4);
    }
    return true;
}

// -----------------------------------------------------------------------------
// Thermal sensor
// -----------------------------------------------------------------------------

static bool init_thermal() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

    if (!mlx.begin(MLX90640_I2C_ADDR, &Wire)) {
        log_w("MLX90640 not found on user I2C (SDA=%d SCL=%d)",
              PIN_I2C_SDA, PIN_I2C_SCL);
        return false;
    }
    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);

    mlx90640_refreshrate_t rate;
    switch (MLX90640_REFRESH_HZ) {
        case 1:  rate = MLX90640_1_HZ;  break;
        case 2:  rate = MLX90640_2_HZ;  break;
        case 4:  rate = MLX90640_4_HZ;  break;
        case 8:  rate = MLX90640_8_HZ;  break;
        case 16: rate = MLX90640_16_HZ; break;
        case 32: rate = MLX90640_32_HZ; break;
        case 64: rate = MLX90640_64_HZ; break;
        default: rate = MLX90640_8_HZ;  break;
    }
    mlx.setRefreshRate(rate);
    log_i("MLX90640 ready at %d Hz", MLX90640_REFRESH_HZ);
    return true;
}

// Dedicated task so the camera loop never waits on the slow thermal readout.
static void thermal_task(void *) {
    float local[32 * 24];
    for (;;) {
        if (mlx.getFrame(local) == 0) {
            float mn = local[0], mx = local[0];
            for (int i = 1; i < 32 * 24; ++i) {
                if (local[i] < mn) mn = local[i];
                if (local[i] > mx) mx = local[i];
            }
            if (xSemaphoreTake(thermal_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                memcpy(thermal_frame, local, sizeof(local));
                thermal_min = mn;
                thermal_max = mx;
                thermal_seq++;
                xSemaphoreGive(thermal_mutex);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        // Cap the loop so we don't starve other tasks even at high refresh.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// -----------------------------------------------------------------------------
// HTTP handlers
// -----------------------------------------------------------------------------

static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "30");

    char part_buf[64];
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { res = ESP_FAIL; break; }

        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
    }
    return res;
}

static esp_err_t snapshot_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snap.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t thermal_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!mlx_ok) {
        const char *msg = "{\"error\":\"mlx90640 not detected\"}";
        return httpd_resp_send(req, msg, strlen(msg));
    }

    // Roughly 32*24*6 chars for values + overhead. Allocate generously.
    const size_t cap = 6500;
    char *buf = (char *)malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    float mn, mx;
    uint32_t seq;
    float snap[32 * 24];
    if (xSemaphoreTake(thermal_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    memcpy(snap, thermal_frame, sizeof(snap));
    mn = thermal_min; mx = thermal_max; seq = thermal_seq;
    xSemaphoreGive(thermal_mutex);

    int off = snprintf(buf, cap,
        "{\"w\":32,\"h\":24,\"seq\":%u,\"min\":%.2f,\"max\":%.2f,\"data\":[",
        seq, mn, mx);
    for (int i = 0; i < 32 * 24 && off < (int)cap - 16; ++i) {
        off += snprintf(buf + off, cap - off, "%s%.2f",
                        i == 0 ? "" : ",", snap[i]);
    }
    off += snprintf(buf + off, cap - off, "]}");

    esp_err_t res = httpd_resp_send(req, buf, off);
    free(buf);
    return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    const char *html =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Squirrel cam</title>"
        "<style>body{background:#111;color:#ddd;font-family:sans-serif;"
        "text-align:center;margin:0;padding:1em}img{max-width:100%;}</style>"
        "<h1>Squirrel cam</h1>"
        "<img src='/stream' alt='stream'>"
        "<p><a style='color:#6cf' href='/thermal'>thermal JSON</a> &middot; "
        "<a style='color:#6cf' href='/snapshot'>snapshot</a></p>";
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static void start_http() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.ctrl_port   = 32768;
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    // MJPEG and JSON on the same server would fight over the single socket,
    // so bump the limits a bit.
    cfg.lru_purge_enable = true;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        log_e("httpd_start failed");
        return;
    }

    httpd_uri_t uris[] = {
        { .uri="/",         .method=HTTP_GET, .handler=index_handler,    .user_ctx=nullptr },
        { .uri="/stream",   .method=HTTP_GET, .handler=stream_handler,   .user_ctx=nullptr },
        { .uri="/snapshot", .method=HTTP_GET, .handler=snapshot_handler, .user_ctx=nullptr },
        { .uri="/thermal",  .method=HTTP_GET, .handler=thermal_handler,  .user_ctx=nullptr },
    };
    for (auto &u : uris) httpd_register_uri_handler(server, &u);
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------

static void connect_wifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    log_i("WiFi: connecting to %s", WIFI_SSID);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        log_e("WiFi connect failed; restarting");
        ESP.restart();
    }
    log_i("WiFi: %s  IP=%s", WIFI_SSID, WiFi.localIP().toString().c_str());

    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        log_i("mDNS: http://%s.local/", MDNS_HOSTNAME);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    log_i("squirrel-cam booting");

    thermal_mutex = xSemaphoreCreateMutex();

    if (!init_camera()) {
        log_e("camera init failed; halting");
        while (true) delay(1000);
    }

    mlx_ok = init_thermal();
    if (mlx_ok) {
        xTaskCreatePinnedToCore(thermal_task, "thermal", 4096, nullptr, 1, nullptr, 0);
    }

    connect_wifi();
    start_http();

    log_i("ready: open http://%s/ or http://%s.local/",
          WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
}

void loop() {
    // Watchdog-friendly idle; the camera/thermal/http all live in tasks.
    delay(1000);
    if (WiFi.status() != WL_CONNECTED) {
        log_w("WiFi dropped, reconnecting");
        WiFi.reconnect();
    }
}
