#ifndef MAX30102_H
#define MAX30102_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define MAX30102_ADDR  0x57

typedef struct {
    i2c_master_dev_handle_t dev_handle;
    uint32_t red_val;
    uint32_t ir_val;
    bool finger_detected;
    int heart_rate_bpm;
    int16_t wave_val;
    
    // Internal beat detection state
    uint32_t ir_dc_avg;
    int32_t last_ir_diff;
    int64_t last_beat_time_ms;
    int bpm_history[4];
    uint8_t bpm_idx;
} max30102_t;

esp_err_t max30102_init(i2c_master_bus_handle_t bus_handle, uint8_t dev_addr, max30102_t *dev);
esp_err_t max30102_read_sample(max30102_t *dev);
void max30102_process_heart_rate(max30102_t *dev, int64_t now_ms);

#endif // MAX30102_H

