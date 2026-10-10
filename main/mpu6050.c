#include "mpu6050.h"
#include <math.h>
#include "esp_log.h"

static const char *TAG = "MPU6050";

#define REG_SMPLRT_DIV   0x19
#define REG_CONFIG       0x1A
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1   0x6B
#define REG_WHO_AM_I     0x75

// Sensitivity for +/- 16G (2048 LSB/g)
#define ACCEL_SCALE_16G  2048.0f
// Sensitivity for +/- 500 deg/s
#define GYRO_SCALE_500   65.5f
#define GRAVITY_MSS      9.80665f

static esp_err_t write_reg(mpu6050_t *dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev->dev_handle, buf, 2, 100);
}

static esp_err_t read_regs(mpu6050_t *dev, uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(dev->dev_handle, &reg, 1, data, len, 100);
}

esp_err_t mpu6050_init(i2c_master_bus_handle_t bus_handle, uint8_t dev_addr, mpu6050_t *dev) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add MPU6050 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t who_am_i = 0;
    ret = read_regs(dev, REG_WHO_AM_I, &who_am_i, 1);
    if (ret != ESP_OK || who_am_i != 0x68) {
        ESP_LOGW(TAG, "WHO_AM_I responded with 0x%02X (expected 0x68)", who_am_i);
    }

    // Wake up MPU6050 (clear sleep bit in PWR_MGMT_1)
    write_reg(dev, REG_PWR_MGMT_1, 0x01); // Clock source = PLL with X Gyro
    write_reg(dev, REG_CONFIG, 0x03);     // DLPF 42Hz bandwidth
    write_reg(dev, REG_SMPLRT_DIV, 0x04); // Sample rate divider
    write_reg(dev, REG_GYRO_CONFIG, 0x08); // Gyro +/- 500 deg/s
    write_reg(dev, REG_ACCEL_CONFIG, 0x18); // Accel +/- 16G range

    ESP_LOGI(TAG, "MPU6050 initialized successfully (Range: +/-16G)");
    return ESP_OK;
}

esp_err_t mpu6050_read_data(mpu6050_t *dev) {
    uint8_t data[14];
    esp_err_t ret = read_regs(dev, REG_ACCEL_XOUT_H, data, 14);
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t raw_ax = (int16_t)((data[0] << 8) | data[1]);
    int16_t raw_ay = (int16_t)((data[2] << 8) | data[3]);
    int16_t raw_az = (int16_t)((data[4] << 8) | data[5]);
    // data[6], [7] is temperature
    int16_t raw_gx = (int16_t)((data[8] << 8) | data[9]);
    int16_t raw_gy = (int16_t)((data[10] << 8) | data[11]);
    int16_t raw_gz = (int16_t)((data[12] << 8) | data[13]);

    float ax_g = (float)raw_ax / ACCEL_SCALE_16G;
    float ay_g = (float)raw_ay / ACCEL_SCALE_16G;
    float az_g = (float)raw_az / ACCEL_SCALE_16G;

    dev->total_g = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
    dev->accel_x = ax_g * GRAVITY_MSS;
    dev->accel_y = ay_g * GRAVITY_MSS;
    dev->accel_z = az_g * GRAVITY_MSS;

    dev->gyro_x = (float)raw_gx / GYRO_SCALE_500;
    dev->gyro_y = (float)raw_gy / GYRO_SCALE_500;
    dev->gyro_z = (float)raw_gz / GYRO_SCALE_500;

    return ESP_OK;
}

