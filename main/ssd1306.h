#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64

typedef struct {
    i2c_master_dev_handle_t dev_handle;
    uint8_t buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];
} ssd1306_t;

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus_handle, uint8_t dev_addr, ssd1306_t *dev);
void ssd1306_clear(ssd1306_t *dev);
void ssd1306_draw_pixel(ssd1306_t *dev, int16_t x, int16_t y, bool color);
void ssd1306_draw_line(ssd1306_t *dev, int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool color);
void ssd1306_fill_rect(ssd1306_t *dev, int16_t x, int16_t y, int16_t w, int16_t h, bool color);
void ssd1306_draw_string(ssd1306_t *dev, int16_t x, int16_t y, const char *str, bool color, bool invert);
void ssd1306_draw_string_scaled(ssd1306_t *dev, int16_t x, int16_t y, const char *str, uint8_t scale, bool color);
esp_err_t ssd1306_refresh(ssd1306_t *dev);

#endif // SSD1306_H

