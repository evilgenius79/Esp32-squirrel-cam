#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define THERMAL_W 32
#define THERMAL_H 24

// Bring up the MLX90640 and start the background refresh task. Returns
// ESP_FAIL if the sensor isn't detected; callers should treat that as
// non-fatal and expose the failure through the HTTP API instead of
// refusing to boot.
esp_err_t thermal_start(void);

// True if thermal_start found the sensor.
bool thermal_available(void);

// Copy the latest 32x24 frame (in Celsius, row-major, sensor orientation)
// out to `dst`. Also returns min/max/sequence. `dst` must point to at
// least THERMAL_W*THERMAL_H floats.
//
// Returns ESP_OK if a frame was copied, ESP_ERR_NOT_FOUND if no frame has
// been captured yet, ESP_ERR_TIMEOUT if the mutex couldn't be taken.
esp_err_t thermal_snapshot(float *dst, float *tmin, float *tmax, uint32_t *seq,
                           uint32_t timeout_ms);
