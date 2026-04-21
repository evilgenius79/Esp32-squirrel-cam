#pragma once
#include "esp_err.h"

// Initialises esp_wifi_remote in station mode and blocks until the
// co-processor has associated to the configured AP and we have an IP.
esp_err_t wifi_start_blocking(void);
