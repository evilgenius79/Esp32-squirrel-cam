#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Bring the MIPI-CSI camera up and spin a capture+encode task. After this
// returns OK, camera_get_jpeg() yields JPEG frames.
esp_err_t camera_start(void);

// Borrow the latest JPEG frame. The returned pointer is valid until the
// matching camera_release() call. Blocks up to timeout_ms for a frame.
// Returns ESP_OK on success and fills *buf / *len.
esp_err_t camera_get_jpeg(const uint8_t **buf, size_t *len, uint32_t timeout_ms);
void      camera_release(void);
