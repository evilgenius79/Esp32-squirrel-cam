#pragma once

// ---------------------------------------------------------------------------
// User-facing configuration for the ESP32-P4-Nano squirrel cam firmware.
// Override any of these via -D flags or an idf.py menuconfig entry if you
// prefer; keeping them here means you don't need to touch the build system
// for the common tweaks.
// ---------------------------------------------------------------------------

#ifndef WIFI_SSID
#define WIFI_SSID     "your-ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-password"
#endif

#define MDNS_HOSTNAME "squirrelcam"
#define HTTP_PORT     80

// MLX90640 lives on the Nano's general-purpose I2C header.
// Pin map sourced from the Waveshare ESP32-P4-Nano schematic.
#define MLX_I2C_PORT    I2C_NUM_0
#define MLX_I2C_SDA_GPIO 7
#define MLX_I2C_SCL_GPIO 8
#define MLX_I2C_ADDR    0x33
#define MLX_I2C_HZ      400000   // 400 kHz is plenty at 8 Hz thermal refresh

// Thermal refresh rate (Hz). Valid MLX90640 values: 0.5, 1, 2, 4, 8, 16, 32, 64.
#define MLX_REFRESH_HZ  8

// Camera output.
// OV5647 supports up to 2592x1944. Streaming 720p MJPEG over WiFi is a
// comfortable default; the hardware JPEG encoder on the P4 handles it
// without breaking a sweat.
#define CAM_WIDTH       1280
#define CAM_HEIGHT      720
#define CAM_JPEG_Q      70

// How many camera buffers to cycle. 3 lets one frame be in-flight over HTTP
// while the next is being captured and the third is being encoded.
#define CAM_FB_COUNT    3
