idf_component_register(SRCS "main.c" "ssd1306.c" "mpu6050.c" "max30102.c" "gps_nmea.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_http_server esp_driver_i2c esp_driver_gpio esp_driver_uart esp_timer lwip)
