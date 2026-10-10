#include "max30102.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "MAX30102";

#define REG_INTR_STATUS_1 0x00
#define REG_FIFO_WR_PTR   0x04
#define REG_OVF_COUNTER   0x05
#define REG_FIFO_RD_PTR   0x06
#define REG_FIFO_DATA     0x07
#define REG_FIFO_CONFIG   0x08
#define REG_MODE_CONFIG   0x09
#define REG_SPO2_CONFIG   0x0A
#define REG_LED1_PA       0x0C // Red
#define REG_LED2_PA       0x0D // IR
#define REG_PART_ID       0xFF

static esp_err_t write_reg(max30102_t *dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev->dev_handle, buf, 2, 100);
}

static esp_err_t read_regs(max30102_t *dev, uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(dev->dev_handle, &reg, 1, data, len, 100);
}

esp_err_t max30102_init(i2c_master_bus_handle_t bus_handle, uint8_t dev_addr, max30102_t *dev) {
    memset(dev, 0, sizeof(max30102_t));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add MAX30102 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t part_id = 0;
    read_regs(dev, REG_PART_ID, &part_id, 1);
    ESP_LOGI(TAG, "MAX30102 Part ID: 0x%02X", part_id);

    // Reset device
    write_reg(dev, REG_MODE_CONFIG, 0x40);
    // wait brief moment for reset
    vTaskDelay(pdMS_TO_TICKS(10));

    // FIFO config: sample average = 4, fifo rollover enabled
    write_reg(dev, REG_FIFO_CONFIG, 0x4F);

    // Mode config: SpO2 mode (Red + IR enabled)
    write_reg(dev, REG_MODE_CONFIG, 0x03);

    // SpO2 config: ADC range 4096nA, 100 samples/sec, 411us pulse width (18-bit resolution)
    write_reg(dev, REG_SPO2_CONFIG, 0x27);

    // LED pulse amplitudes (LED current ~7.2mA for Red and ~7.2mA for IR)
    write_reg(dev, REG_LED1_PA, 0x24); // Red LED
    write_reg(dev, REG_LED2_PA, 0x24); // IR LED

    // Clear FIFO pointers
    write_reg(dev, REG_FIFO_WR_PTR, 0x00);
    write_reg(dev, REG_OVF_COUNTER, 0x00);
    write_reg(dev, REG_FIFO_RD_PTR, 0x00);

    ESP_LOGI(TAG, "MAX30102 initialized successfully in SpO2/Pulse mode");
    return ESP_OK;
}

esp_err_t max30102_read_sample(max30102_t *dev) {
    uint8_t raw[6];
    esp_err_t ret = read_regs(dev, REG_FIFO_DATA, raw, 6);
    if (ret != ESP_OK) {
        return ret;
    }

    dev->red_val = (((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | (uint32_t)raw[2]) & 0x03FFFF;
    dev->ir_val  = (((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 8) | (uint32_t)raw[5]) & 0x03FFFF;

    // Contact threshold (if IR is below 50000, no finger/wrist contact)
    if (dev->ir_val > 50000) {
        dev->finger_detected = true;
    } else {
        dev->finger_detected = false;
        dev->heart_rate_bpm = 0;
        dev->ir_dc_avg = 0;
        dev->last_beat_time_ms = 0;
    }

    return ESP_OK;
}

void max30102_process_heart_rate(max30102_t *dev, int64_t now_ms) {
    if (!dev->finger_detected) return;

    // Initialize DC filter tracking on first contact
    if (dev->ir_dc_avg == 0) {
        dev->ir_dc_avg = dev->ir_val;
        return;
    }

    // Low pass filter for DC baseline
    dev->ir_dc_avg = (dev->ir_dc_avg * 15 + dev->ir_val) / 16;
    int32_t ac_signal = (int32_t)dev->ir_val - (int32_t)dev->ir_dc_avg;

    // Peak detection: look for zero crossing of derivative / rising edge exceeding AC threshold
    int32_t diff = ac_signal - dev->last_ir_diff;
    dev->last_ir_diff = ac_signal;

    // Compute normalized waveform value for OLED ECG graph
    int32_t scaled = ac_signal / 15;
    if (scaled > 18) scaled = 18;
    if (scaled < -18) scaled = -18;
    dev->wave_val = (int16_t)scaled;

    if (ac_signal > 150 && diff < 0 && dev->last_beat_time_ms > 0) {
        int64_t delta_ms = now_ms - dev->last_beat_time_ms;
        // Check realistic human heart rate period (280ms to 1700ms -> ~35 to 210 BPM)
        if (delta_ms >= 300 && delta_ms <= 1700) {
            int current_bpm = (int)(60000 / delta_ms);
            dev->bpm_history[dev->bpm_idx] = current_bpm;
            dev->bpm_idx = (dev->bpm_idx + 1) % 4;

            int sum = 0, count = 0;
            for (int i = 0; i < 4; i++) {
                if (dev->bpm_history[i] > 0) {
                    sum += dev->bpm_history[i];
                    count++;
                }
            }
            if (count > 0) {
                dev->heart_rate_bpm = sum / count;
            }
            dev->last_beat_time_ms = now_ms;
        }
    } else if (ac_signal > 150 && diff < 0 && dev->last_beat_time_ms == 0) {
        dev->last_beat_time_ms = now_ms;
    }
}

