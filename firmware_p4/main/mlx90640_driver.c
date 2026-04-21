// Minimal MLX90640 driver — see mlx90640_driver.h for the accuracy caveat.
//
// Register map reminders (all 16-bit big-endian over I2C):
//   0x2400..0x273F : 1664 bytes of factory calibration EEPROM
//   0x0400..0x073F : 1664 bytes of per-pixel IR data for the current subpage
//   0x0700         : Ta_Vbe (ambient temperature sensor raw)
//   0x070A         : Gain_raw
//   0x0720         : Tgc / CP
//   0x8000         : Status register (bit 3 = new data, bits 0..2 = last subpage)
//   0x800D         : Control register (refresh rate, ADC resolution, reading pattern)

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mlx90640_driver.h"

static const char *TAG = "mlx";

// ---------------------------------------------------------------------------
// Low-level I2C helpers. The MLX90640 uses 16-bit register addresses and
// 16-bit register values, both big-endian on the wire.
// ---------------------------------------------------------------------------

static esp_err_t read16(mlx90640_t *m, uint16_t addr, uint16_t *out) {
    uint8_t ab[2] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF) };
    uint8_t rb[2];
    esp_err_t err = i2c_master_transmit_receive(m->dev, ab, 2, rb, 2,
                                                pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;
    *out = ((uint16_t)rb[0] << 8) | rb[1];
    return ESP_OK;
}

static esp_err_t write16(mlx90640_t *m, uint16_t addr, uint16_t val) {
    uint8_t b[4] = {
        (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
        (uint8_t)(val  >> 8), (uint8_t)(val  & 0xFF),
    };
    return i2c_master_transmit(m->dev, b, 4, pdMS_TO_TICKS(100));
}

static esp_err_t read_block(mlx90640_t *m, uint16_t addr, uint16_t *out, size_t words) {
    // MLX90640 auto-increments on reads, so we can blast large blocks.
    uint8_t ab[2] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF) };
    uint8_t *rb = (uint8_t *)out;          // we'll byte-swap in place after
    esp_err_t err = i2c_master_transmit_receive(m->dev, ab, 2, rb, words * 2,
                                                pdMS_TO_TICKS(500));
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < words; ++i) {
        uint8_t hi = rb[i*2], lo = rb[i*2 + 1];
        out[i] = ((uint16_t)hi << 8) | lo;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Init / config
// ---------------------------------------------------------------------------

esp_err_t mlx90640_init(mlx90640_t *m, i2c_master_bus_handle_t bus,
                        uint16_t i2c_addr, uint32_t clk_hz) {
    memset(m, 0, sizeof(*m));
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = i2c_addr,
        .scl_speed_hz    = clk_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &m->dev);
    if (err != ESP_OK) return err;

    // Cache EEPROM.
    err = read_block(m, 0x2400, m->ee, 832);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EEPROM read failed: %s", esp_err_to_name(err));
        return err;
    }

    // Simplified linearisation constants (see datasheet sect. 11).
    // These rough values are enough to recover a relative temperature
    // image; the full Melexis math uses per-pixel offset/alpha/Kta/Kv
    // terms from EEPROM for absolute accuracy.
    m->vdd0 = 3.3f;
    m->ta0  = 25.0f;
    // Gain word at EEPROM offset 0x0AA (word 0xAA from 0x2400 base).
    m->gain = (int16_t)m->ee[0x0AA];
    m->emissivity = 1.0f;

    ESP_LOGI(TAG, "mlx90640 detected, gain=%.0f", m->gain);
    return ESP_OK;
}

esp_err_t mlx90640_set_refresh_hz(mlx90640_t *m, uint8_t hz) {
    // Control register at 0x800D: bits [9:7] = refresh rate.
    //   000=0.5Hz 001=1Hz 010=2Hz 011=4Hz 100=8Hz 101=16Hz 110=32Hz 111=64Hz
    uint16_t bits;
    switch (hz) {
        case 1:  bits = 0b001; break;
        case 2:  bits = 0b010; break;
        case 4:  bits = 0b011; break;
        case 8:  bits = 0b100; break;
        case 16: bits = 0b101; break;
        case 32: bits = 0b110; break;
        case 64: bits = 0b111; break;
        default: bits = 0b100; break;       // 8 Hz
    }
    uint16_t ctrl;
    esp_err_t err = read16(m, 0x800D, &ctrl);
    if (err != ESP_OK) return err;
    ctrl = (ctrl & ~(0b111 << 7)) | (bits << 7);
    return write16(m, 0x800D, ctrl);
}

// ---------------------------------------------------------------------------
// Frame read
// ---------------------------------------------------------------------------

// The sensor updates odd and even "subpages" on alternate frames in chess
// mode. The status register bit 3 goes high when a new subpage is ready.
// For visualisation we just read both subpages whenever they become
// available and blit them into a single 32x24 output.

esp_err_t mlx90640_read_frame(mlx90640_t *m, float *out,
                              float *out_tmin, float *out_tmax) {
    static uint16_t frame[834];         // 832 pixels + aux
    int subpages_captured = 0;
    uint32_t start = xTaskGetTickCount();

    while (subpages_captured < 2) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(1500)) {
            return ESP_ERR_TIMEOUT;
        }
        uint16_t status;
        if (read16(m, 0x8000, &status) != ESP_OK) continue;
        if (!(status & 0x0008)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        // Clear "new data" flag before the read so we don't miss the next.
        write16(m, 0x8000, status & ~0x0008);

        if (read_block(m, 0x0400, frame, 832) != ESP_OK) continue;
        uint16_t aux[64];
        read_block(m, 0x0700, aux, 64);

        // Simplified per-pixel: raw signed value scaled by gain, offset by
        // ambient. Enough for a heatmap; swap in the Melexis math if you
        // need absolute accuracy.
        int sp = status & 0x0001;    // which subpage this read belongs to
        float gain_scale = 1.0f / (m->gain == 0 ? 1.0f : fabsf(m->gain));
        float ta = m->ta0 + ((int16_t)aux[0] & 0x03FF) * 0.1f;
        for (int i = 0; i < 32 * 24; ++i) {
            // Only overwrite pixels that belong to this subpage (chess
            // pattern: even i = subpage 0, odd i = subpage 1, roughly).
            if (((i / 32 + i) & 1) != sp) continue;
            int16_t raw = (int16_t)frame[i];
            out[i] = ta + raw * gain_scale * 0.1f;
        }
        subpages_captured++;
    }

    float mn = out[0], mx = out[0];
    for (int i = 1; i < 32 * 24; ++i) {
        if (out[i] < mn) mn = out[i];
        if (out[i] > mx) mx = out[i];
    }
    if (out_tmin) *out_tmin = mn;
    if (out_tmax) *out_tmax = mx;
    return ESP_OK;
}
