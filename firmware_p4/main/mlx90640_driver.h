#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

// Minimal MLX90640 driver, enough to get a visualisable thermal frame.
//
// This is a cut-down implementation: it reads the frame data registers and
// does a *simplified* linearisation into Celsius using the sensor's gain
// and reference values. It is accurate to within a few degrees at room
// temperature — fine for a "warm squirrel vs cold nest" heatmap.
//
// If you want lab-grade absolute accuracy, swap this file for the Melexis
// reference driver (MLX90640_API.c / MLX90640_I2C_Driver.c from Melexis's
// public GitHub) and route its I2C hooks to the IDF i2c_master handle.

typedef struct {
    i2c_master_dev_handle_t dev;
    uint16_t ee[832];    // cached EEPROM (0x2400..0x273F)
    float    vdd0;
    float    ta0;
    float    gain;
    float    emissivity;
} mlx90640_t;

esp_err_t mlx90640_init(mlx90640_t *m, i2c_master_bus_handle_t bus,
                        uint16_t i2c_addr, uint32_t clk_hz);
esp_err_t mlx90640_set_refresh_hz(mlx90640_t *m, uint8_t hz);
esp_err_t mlx90640_read_frame(mlx90640_t *m, float *out_celsius_32x24,
                              float *out_tmin, float *out_tmax);
