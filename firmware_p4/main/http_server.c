// HTTP server: serves the embedded webpage plus MJPEG video, single-shot
// JPEG snapshots, and 32x24 thermal JSON.
//
// Security model: LAN-trust. No auth, no TLS. Same hardening as the XIAO
// firmware: static JSON scratch buffer (no per-request malloc), bounded
// appends, explicit 500 on overflow.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "camera.h"
#include "thermal.h"
#include "http_server.h"

static const char *TAG = "http";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server;

// ---------------------------------------------------------------------------
// Shared /thermal scratch buffer
// ---------------------------------------------------------------------------

#define THERMAL_JSON_CAP 8192
static char               s_json_buf[THERMAL_JSON_CAP];
static SemaphoreHandle_t  s_json_mux;

static bool json_appendf(char *buf, size_t cap, int *off, const char *fmt, ...) {
    if (*off < 0 || (size_t)*off >= cap) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - (size_t)*off) return false;
    *off += n;
    return true;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t h_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static const char *MJPEG_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *MJPEG_BDY  = "\r\n--frame\r\n";
static const char *MJPEG_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t h_stream(httpd_req_t *req) {
    httpd_resp_set_type(req, MJPEG_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part[64];
    esp_err_t res = ESP_OK;
    uint32_t epoch = 0;     // per-connection cookie: blocks until a fresh
                            // frame is available, so we never re-send the
                            // same JPEG twice and never busy-loop.
    while (1) {
        const uint8_t *buf;
        size_t len;
        if (camera_get_jpeg(&buf, &len, &epoch, 2000) != ESP_OK) {
            res = ESP_FAIL;
            break;
        }
        int hlen = snprintf(part, sizeof(part), MJPEG_PART, (unsigned)len);
        if (hlen <= 0 || hlen >= (int)sizeof(part)) {
            res = ESP_FAIL;
            break;
        }
        res = httpd_resp_send_chunk(req, MJPEG_BDY, strlen(MJPEG_BDY));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)buf, len);
        if (res != ESP_OK) break;
    }
    return res;
}

static esp_err_t h_snapshot(httpd_req_t *req) {
    const uint8_t *buf;
    size_t len;
    if (camera_get_jpeg(&buf, &len, NULL, 2000) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snap.jpg");
    return httpd_resp_send(req, (const char *)buf, len);
}

static esp_err_t h_thermal(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!thermal_available()) {
        const char *msg = "{\"error\":\"mlx90640 not detected\"}";
        return httpd_resp_send(req, msg, strlen(msg));
    }

    if (xSemaphoreTake(s_json_mux, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_FAIL;
    }

    float frame[THERMAL_W * THERMAL_H], tmin, tmax;
    uint32_t seq;
    if (thermal_snapshot(frame, &tmin, &tmax, &seq, 200) != ESP_OK) {
        xSemaphoreGive(s_json_mux);
        const char *msg = "{\"error\":\"no frame yet\"}";
        return httpd_resp_send(req, msg, strlen(msg));
    }

    int off = 0;
    bool ok = json_appendf(s_json_buf, THERMAL_JSON_CAP, &off,
        "{\"w\":%d,\"h\":%d,\"seq\":%u,\"min\":%.2f,\"max\":%.2f,\"data\":[",
        THERMAL_W, THERMAL_H, (unsigned)seq, tmin, tmax);
    for (int i = 0; i < THERMAL_W * THERMAL_H && ok; ++i) {
        ok = json_appendf(s_json_buf, THERMAL_JSON_CAP, &off,
                          "%s%.2f", i == 0 ? "" : ",", frame[i]);
    }
    if (ok) ok = json_appendf(s_json_buf, THERMAL_JSON_CAP, &off, "]}");

    esp_err_t res;
    if (!ok) {
        res = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json overflow");
    } else {
        res = httpd_resp_send(req, s_json_buf, off);
    }
    xSemaphoreGive(s_json_mux);
    return res;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

esp_err_t http_server_start(void) {
    s_json_mux = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port     = HTTP_PORT;
    cfg.ctrl_port       = 32768;
    cfg.max_uri_handlers = 8;
    cfg.stack_size      = 10240;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t routes[] = {
        { "/",         HTTP_GET, h_index,    NULL },
        { "/stream",   HTTP_GET, h_stream,   NULL },
        { "/snapshot", HTTP_GET, h_snapshot, NULL },
        { "/thermal",  HTTP_GET, h_thermal,  NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    return ESP_OK;
}
