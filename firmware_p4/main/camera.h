#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Bring the MIPI-CSI camera up and spin a capture+encode task. After this
// returns OK, camera_get_jpeg() yields JPEG frames.
esp_err_t camera_start(void);

// Borrow the latest JPEG frame.
//
// `epoch_io` is an in/out cookie the caller uses to track frame freshness:
// pass `*epoch_io == 0` on the first call to get whatever is latest; on
// subsequent calls the function blocks (up to timeout_ms) until a frame
// newer than *epoch_io is published, then updates *epoch_io. Pass NULL to
// always get the latest published frame without waiting.
//
// Multiple readers can call this concurrently — there is no per-reader
// lock. The returned pointer is valid until the writer rotates back
// around the slot ring (≥3 frame periods); a slow reader can in theory
// see a torn frame, which for MJPEG just means one bad frame followed by
// recovery on the next.
esp_err_t camera_get_jpeg(const uint8_t **buf, size_t *len,
                          uint32_t *epoch_io, uint32_t timeout_ms);

