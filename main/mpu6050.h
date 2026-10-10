#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define MPU6050_ADDR_DEFAULT  0x68
#define MPU6050_ADDR_ALT      0x69

typedef struct {
    i2c_master_dev_handle_t dev_handle;
    float accel_x;   // m/s^2
    float accel_y;   // m/s^2
    float accel_z;   // m/s^2
    float total_g;   // Gs
    float gyro_x;    // deg/s
    float gyro_y;    // deg/s
    float gyro_z;    // deg/s
} mpu6050_t;

esp_err_t mpu6050_init(i2c_master_bus_handle_t bus_handle, uint8_t dev_addr, mpu6050_t *dev);
esp_err_t mpu6050_read_data(mpu6050_t *dev);

#endif // MPU6050_H

