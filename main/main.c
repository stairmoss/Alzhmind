/*
 * =========================================================================================
 * Project: Alzhmind - Wearable Health & Safety System for Alzheimer's Patients
 * Framework: Native ESP-IDF (C)
 * Target: ESP32
 * 
 * Hardware Pinout Mapping:
 *   - Shared I2C Bus: SDA -> GPIO 21, SCL -> GPIO 22
 *       * OLED Display 128x64 (SSD1306, 0x3C)
 *       * HW-605 / MAX30102 Heart Rate & Pulse Oximeter (0x57)
 *       * MPU6050 6-DOF Accelerometer & Gyroscope (0x68)
 *   - NEO-6M GPS Module (UART2):
 *       * TX -> GPIO 16 (ESP32 RX2)
 *       * RX -> GPIO 17 (ESP32 TX2)
 *   - Active Buzzer: Signal -> GPIO 12
 *   - Alert LED:     Anode  -> GPIO 14 (with resistor)
 *   - WiFi: Station + SoftAP with Embedded HTTP Telemetry Server (Port 80)
 * =========================================================================================
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

// Hardware Drivers
#include "ssd1306.h"
#include "mpu6050.h"
#include "max30102.h"
#include "gps_nmea.h"

static const char *TAG = "ALZHMIND_MAIN";

// ======================== PIN DEFINITIONS ========================
#define I2C_SDA_PIN         GPIO_NUM_21
#define I2C_SCL_PIN         GPIO_NUM_22
#define GPS_UART_PORT       UART_NUM_2
#define GPS_RX_PIN          GPIO_NUM_16 // ESP32 RX connects to GPS TX
#define GPS_TX_PIN          GPIO_NUM_17 // ESP32 TX connects to GPS RX
#define BUZZER_PIN          GPIO_NUM_12 // Active Buzzer
#define LED_PIN             GPIO_NUM_14 // Alert LED

// ======================== FALL DETECTION =========================
#define FALL_G_THRESHOLD    10.0f       // 10G impact threshold
#define FALL_ALARM_MS       5000        // Alarm duration: 5 seconds

// ======================== UDP BROADCAST ==========================
#define UDP_BROADCAST_PORT  5005        // Dedicated Wi-Fi telemetry broadcast port

// ======================== WIFI CONFIGURATION =====================
/* Do not commit household Wi-Fi credentials. Configure these via an ESP-IDF
 * build-time secret/configuration before deploying. Empty STA credentials keep
 * the device in local SoftAP fallback mode. */
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID       ""
#endif
#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS       ""
#endif
#define WIFI_AP_SSID        "Alzhmind-Wearable"
#define WIFI_AP_PASS        "CHANGE-ME-BEFORE-DEPLOYMENT"

// ======================== SHARED SYSTEM STATE ====================
typedef struct {
    // Vitals
    int heart_rate_bpm;
    bool finger_detected;
    int16_t wave_sample;

    // Motion & Fall
    float accel_x;
    float accel_y;
    float accel_z;
    float total_g;
    bool fall_detected;
    int64_t fall_time_ms;

    // GPS
    bool gps_valid;
    double latitude;
    double longitude;
    float speed_kmph;
    int satellites;

    // Networking
    bool wifi_connected;
    char ip_str[32];
    int8_t rssi;
} system_state_t;

static system_state_t g_state;
static SemaphoreHandle_t g_state_mutex = NULL;

// Device handles
static i2c_master_bus_handle_t g_i2c_bus = NULL;
static ssd1306_t g_oled;
static mpu6050_t g_mpu;
static max30102_t g_max;
static httpd_handle_t g_server = NULL;

// Forward Declarations
static void init_peripherals(void);
static void wifi_init_dual_mode(void);
static void start_webserver(void);
static void sensor_task(void *pvParameters);
static void gps_task(void *pvParameters);
static void display_task(void *pvParameters);
static void alarm_task(void *pvParameters);
static void udp_telemetry_task(void *pvParameters);

// ================================================================
//                       APPLICATION ENTRY POINT
// ================================================================
void app_main(void) {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  Alzhmind: Patient Safety System Initializing   ");
    ESP_LOGI(TAG, "==================================================");

    // Initialize State Mutex
    g_state_mutex = xSemaphoreCreateMutex();
    memset(&g_state, 0, sizeof(system_state_t));
    g_state.total_g = 1.0f;
    snprintf(g_state.ip_str, sizeof(g_state.ip_str), "Connecting...");

    // 1. Initialize NVS (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Hardware Peripherals (I2C, UART, GPIO)
    init_peripherals();

    // 3. Initialize Wi-Fi & Start HTTP Telemetry Web Server
    wifi_init_dual_mode();
    start_webserver();

    // 4. Spawn Real-Time FreeRTOS Tasks
    xTaskCreatePinnedToCore(sensor_task,        "sensor_task",   4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(gps_task,           "gps_task",      3072, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(display_task,       "display_task",  4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(alarm_task,         "alarm_task",    2048, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(udp_telemetry_task, "udp_telemetry", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "[OK] System fully booted and running!");
}

// ================================================================
//                   HARDWARE INITIALIZATION
// ================================================================
static void init_peripherals(void) {
    // 1. GPIO Configuration (Active Buzzer & Alert LED)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER_PIN) | (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(BUZZER_PIN, 0);
    gpio_set_level(LED_PIN, 0);

    // Self-test flash
    gpio_set_level(BUZZER_PIN, 1);
    gpio_set_level(LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(BUZZER_PIN, 0);
    gpio_set_level(LED_PIN, 0);
    ESP_LOGI(TAG, "[OK] Buzzer (GPIO 12) & LED (GPIO 14) configured");

    // 2. Shared I2C Master Bus Init (GPIO 21 SDA, GPIO 22 SCL)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &g_i2c_bus));
    ESP_LOGI(TAG, "[OK] Shared I2C Master Bus initialized on SDA:21, SCL:22");

    // 3. Initialize SSD1306 OLED (0x3C)
    if (ssd1306_init(g_i2c_bus, 0x3C, &g_oled) == ESP_OK) {
        ssd1306_clear(&g_oled);
        ssd1306_draw_string(&g_oled, 10, 15, "ALZHMIND SYSTEM", true, false);
        ssd1306_draw_string(&g_oled, 10, 32, "Starting IDF v6...", true, false);
        ssd1306_refresh(&g_oled);
    } else {
        ESP_LOGE(TAG, "[ERROR] SSD1306 OLED initialization failed!");
    }

    // 4. Initialize MPU6050 (0x68)
    if (mpu6050_init(g_i2c_bus, MPU6050_ADDR_DEFAULT, &g_mpu) != ESP_OK) {
        ESP_LOGW(TAG, "Trying MPU6050 alternative address 0x69...");
        mpu6050_init(g_i2c_bus, MPU6050_ADDR_ALT, &g_mpu);
    }

    // 5. Initialize MAX30102 / HW-605 (0x57)
    if (max30102_init(g_i2c_bus, MAX30102_ADDR, &g_max) != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] MAX30102 sensor initialization failed!");
    }

    // 6. Initialize UART2 for NEO-6M GPS (GPIO 16 RX, GPIO 17 TX)
    uart_config_t uart_cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_PORT, GPS_TX_PIN, GPS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_PORT, 1024, 0, 0, NULL, 0));
    gps_nmea_init();
    ESP_LOGI(TAG, "[OK] GPS UART2 initialized on RX:16, TX:17 at 9600 baud");
}

// ================================================================
//                      SENSOR MONITORING TASK
// ================================================================
static void sensor_task(void *pvParameters) {
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        // 1. Read MPU6050 Motion & Detect Falls
        if (mpu6050_read_data(&g_mpu) == ESP_OK) {
            if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_state.accel_x = g_mpu.accel_x;
                g_state.accel_y = g_mpu.accel_y;
                g_state.accel_z = g_mpu.accel_z;
                g_state.total_g = g_mpu.total_g;

                // Impact Fall Threshold Check
                if (g_mpu.total_g >= FALL_G_THRESHOLD) {
                    if (!g_state.fall_detected) {
                        g_state.fall_detected = true;
                        g_state.fall_time_ms = now_ms;
                        ESP_LOGW(TAG, ">>> FALL DETECTED! Impact: %.2f G <<<", g_mpu.total_g);
                    }
                }

                // Check auto-clear duration
                if (g_state.fall_detected && (now_ms - g_state.fall_time_ms > FALL_ALARM_MS)) {
                    g_state.fall_detected = false;
                    ESP_LOGI(TAG, "Fall alarm auto-reset after timeout.");
                }

                xSemaphoreGive(g_state_mutex);
            }
        }

        // 2. Read MAX30102 Heart Rate
        if (max30102_read_sample(&g_max) == ESP_OK) {
            max30102_process_heart_rate(&g_max, now_ms);

            if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_state.finger_detected = g_max.finger_detected;
                g_state.heart_rate_bpm = g_max.heart_rate_bpm;
                g_state.wave_sample = g_max.wave_val;
                xSemaphoreGive(g_state_mutex);
            }
        }

        // Sample rate: ~40 Hz (25 ms)
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(25));
    }
}

// ================================================================
//                       GPS PARSER TASK
// ================================================================
static void gps_task(void *pvParameters) {
    uint8_t rx_buf[128];
    gps_data_t gps_local;
    memset(&gps_local, 0, sizeof(gps_local));

    while (1) {
        int len = uart_read_bytes(GPS_UART_PORT, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                gps_nmea_parse_char((char)rx_buf[i], &gps_local);
            }

            if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_state.gps_valid = gps_local.valid;
                g_state.latitude = gps_local.latitude;
                g_state.longitude = gps_local.longitude;
                g_state.speed_kmph = gps_local.speed_kmph;
                g_state.satellites = gps_local.satellites;
                xSemaphoreGive(g_state_mutex);
            }
        }
    }
}

// ================================================================
//                    ALARM TASK (BUZZER & LED)
// ================================================================
static void alarm_task(void *pvParameters) {
    bool alert_phase = false;

    while (1) {
        bool is_fall = false;
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            is_fall = g_state.fall_detected;
            xSemaphoreGive(g_state_mutex);
        }

        if (is_fall) {
            alert_phase = !alert_phase;
            gpio_set_level(BUZZER_PIN, alert_phase ? 1 : 0);
            gpio_set_level(LED_PIN, alert_phase ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(150));
        } else {
            gpio_set_level(BUZZER_PIN, 0);
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ================================================================
//          UDP TELEMETRY BROADCAST TASK (PORT 5005)
// ================================================================
static void udp_telemetry_task(void *pvParameters) {
    // Wait for Wi-Fi to start
    vTaskDelay(pdMS_TO_TICKS(2000));

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_BROADCAST_PORT);
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST); // Broadcast to 255.255.255.255

    ESP_LOGI(TAG, "[OK] UDP Telemetry Broadcaster listening/transmitting on port %d", UDP_BROADCAST_PORT);

    while (1) {
        system_state_t st;
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            memcpy(&st, &g_state, sizeof(system_state_t));
            xSemaphoreGive(g_state_mutex);
        }

        // Format Complete Real-time Telemetry JSON Packet
        char payload[512];
        int len = snprintf(payload, sizeof(payload),
            "{"
            "\"device_id\":\"ALZHMIND-PATIENT-01\","
            "\"fall_detected\":%s,"
            "\"heart_rate_bpm\":%d,"
            "\"finger_detected\":%s,"
            "\"acceleration\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"total_g\":%.2f},"
            "\"gps\":{\"valid\":%s,\"latitude\":%.6f,\"longitude\":%.6f,\"speed_kmph\":%.1f,\"satellites\":%d},"
            "\"google_maps_url\":\"https://maps.google.com/?q=%.6f,%.6f\","
            "\"uptime_sec\":%lld"
            "}\n",
            st.fall_detected ? "true" : "false",
            st.heart_rate_bpm,
            st.finger_detected ? "true" : "false",
            st.accel_x, st.accel_y, st.accel_z, st.total_g,
            st.gps_valid ? "true" : "false",
            st.latitude, st.longitude, st.speed_kmph, st.satellites,
            st.latitude, st.longitude,
            (long long)(esp_timer_get_time() / 1000000));

        // Send via Wi-Fi UDP Broadcast to all devices on network
        sendto(sock, payload, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        // Continuous Serial Monitor Log: GPS Latitude, Longitude, Vitals, Motion & Fall
        printf("[SERIAL] GPS: Lat=%.6f, Lng=%.6f (Fix:%s, Sats:%d) | HR: %d BPM | Accel: %.2f G | Fall: %s\n",
               st.latitude, st.longitude,
               st.gps_valid ? "VALID" : "SEARCHING",
               st.satellites,
               st.heart_rate_bpm,
               st.total_g,
               st.fall_detected ? "*** FALL ALERT (>=10G) ***" : "NORMAL");

        // Broadcast every 500 ms during a fall alert, 1000 ms normally
        vTaskDelay(pdMS_TO_TICKS(st.fall_detected ? 500 : 1000));
    }
}

// ================================================================
//                    OLED DISPLAY TASK
// ================================================================
// ================================================================
//                    OLED DISPLAY TASK
// ================================================================
#define GRAPH_WIDTH  76
#define BASELINE_Y   24

static void display_task(void *pvParameters) {
    char str_buf[32];
    uint8_t ecg_buf[GRAPH_WIDTH];
    memset(ecg_buf, BASELINE_Y, sizeof(ecg_buf));

    while (1) {
        system_state_t st;
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            memcpy(&st, &g_state, sizeof(system_state_t));
            xSemaphoreGive(g_state_mutex);
        }

        // 1. Shift ECG buffer left by 1
        memmove(&ecg_buf[0], &ecg_buf[1], GRAPH_WIDTH - 1);

        // 2. Compute new Y point for graph
        if (st.finger_detected) {
            int y = BASELINE_Y - st.wave_sample;
            if (y < 4) y = 4;
            if (y > 44) y = 44;
            ecg_buf[GRAPH_WIDTH - 1] = (uint8_t)y;
        } else {
            ecg_buf[GRAPH_WIDTH - 1] = BASELINE_Y;
        }

        ssd1306_clear(&g_oled);

        // 3. Draw ECG Waveform Graph (X: 0 to 75, Y: 4 to 44)
        for (int i = 0; i < GRAPH_WIDTH - 1; i++) {
            ssd1306_draw_line(&g_oled, i, ecg_buf[i], i + 1, ecg_buf[i + 1], true);
        }

        // 4. Vertical dividing line between graph and numbers (X = 77)
        ssd1306_draw_line(&g_oled, 77, 2, 77, 46, true);

        // 5. Heart Rate Numeric Value on the side (X: 81 to 127)
        ssd1306_draw_string(&g_oled, 82, 2, "HEART", true, false);

        if (st.finger_detected && st.heart_rate_bpm > 0) {
            snprintf(str_buf, sizeof(str_buf), "%d", st.heart_rate_bpm);
            ssd1306_draw_string_scaled(&g_oled, 82, 14, str_buf, 2, true);
            ssd1306_draw_string(&g_oled, 85, 34, "BPM", true, false);
        } else {
            ssd1306_draw_string_scaled(&g_oled, 84, 14, "--", 2, true);
            ssd1306_draw_string(&g_oled, 81, 34, st.finger_detected ? "SYNC" : "IDLE", true, false);
        }

        // 6. Horizontal dividing line above bottom section (Y = 47)
        ssd1306_draw_line(&g_oled, 0, 47, 127, 47, true);

        // 7. Motion Status at the bottom (Y: 49 to 63)
        if (st.fall_detected) {
            ssd1306_fill_rect(&g_oled, 0, 48, 128, 16, true);
            snprintf(str_buf, sizeof(str_buf), "! FALL: %.1fG !", st.total_g);
            ssd1306_draw_string(&g_oled, 10, 52, str_buf, false, false);
        } else {
            snprintf(str_buf, sizeof(str_buf), "Motion: %.2f G (Norm)", st.total_g);
            ssd1306_draw_string(&g_oled, 2, 52, str_buf, true, false);
        }

        ssd1306_refresh(&g_oled);
        vTaskDelay(pdMS_TO_TICKS(50)); // 20 FPS for smooth scrolling ECG wave
    }
}

// ================================================================
//                       WIFI INITIALIZATION
// ================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.wifi_connected = false;
            xSemaphoreGive(g_state_mutex);
        }
        esp_wifi_connect();
        ESP_LOGI(TAG, "Reconnecting to WiFi router...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.wifi_connected = true;
            esp_ip4addr_ntoa(&event->ip_info.ip, g_state.ip_str, sizeof(g_state.ip_str));
            xSemaphoreGive(g_state_mutex);
        }
        ESP_LOGI(TAG, "WiFi Connected! IP Address: %s", g_state.ip_str);
    }
}

static void wifi_init_dual_mode(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_STA_SSID,
            .password = WIFI_STA_PASS,
        },
    };

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .password = WIFI_AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Record fallback AP IP (192.168.4.1)
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    if (g_state.ip_str[0] == '\0' || strcmp(g_state.ip_str, "Connecting...") == 0) {
        esp_ip4addr_ntoa(&ip_info.ip, g_state.ip_str, sizeof(g_state.ip_str));
    }
    ESP_LOGI(TAG, "WiFi Started in AP+STA mode. SoftAP SSID: %s (IP: 192.168.4.1)", WIFI_AP_SSID);
}

// ================================================================
//                   HTTP SERVER HANDLERS (LAPTOP)
// ================================================================

// GET /api/data -> Real-time JSON Telemetry
static esp_err_t api_data_get_handler(httpd_req_t *req) {
    system_state_t st;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(&st, &g_state, sizeof(system_state_t));
        xSemaphoreGive(g_state_mutex);
    }

    char json_buf[512];
    int len = snprintf(json_buf, sizeof(json_buf),
        "{"
        "\"device_id\":\"ALZHMIND-PATIENT-01\","
        "\"fall_detected\":%s,"
        "\"heart_rate_bpm\":%d,"
        "\"finger_detected\":%s,"
        "\"acceleration\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"total_g\":%.2f},"
        "\"gps\":{\"valid\":%s,\"latitude\":%.6f,\"longitude\":%.6f,\"speed_kmph\":%.1f,\"satellites\":%d},"
        "\"uptime_sec\":%lld"
        "}",
        st.fall_detected ? "true" : "false",
        st.heart_rate_bpm,
        st.finger_detected ? "true" : "false",
        st.accel_x, st.accel_y, st.accel_z, st.total_g,
        st.gps_valid ? "true" : "false",
        st.latitude, st.longitude, st.speed_kmph, st.satellites,
        (long long)(esp_timer_get_time() / 1000000));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json_buf, len);

    return ESP_OK;
}

// POST /api/reset_fall -> Caregiver reset
static esp_err_t api_reset_fall_handler(httpd_req_t *req) {
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_state.fall_detected = false;
        xSemaphoreGive(g_state_mutex);
    }
    gpio_set_level(BUZZER_PIN, 0);
    gpio_set_level(LED_PIN, 0);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// GET / -> Live Web Dashboard
static esp_err_t root_get_handler(httpd_req_t *req) {
    extern const char dashboard_html[];
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, dashboard_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    httpd_uri_t uri_root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t uri_data = {
        .uri       = "/api/data",
        .method    = HTTP_GET,
        .handler   = api_data_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t uri_telemetry = {
        .uri       = "/api/telemetry",
        .method    = HTTP_GET,
        .handler   = api_data_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t uri_reset = {
        .uri       = "/api/reset_fall",
        .method    = HTTP_POST,
        .handler   = api_reset_fall_handler,
        .user_ctx  = NULL
    };

    if (httpd_start(&g_server, &config) == ESP_OK) {
        httpd_register_uri_handler(g_server, &uri_root);
        httpd_register_uri_handler(g_server, &uri_data);
        httpd_register_uri_handler(g_server, &uri_telemetry);
        httpd_register_uri_handler(g_server, &uri_reset);
        ESP_LOGI(TAG, "[OK] HTTP Telemetry Server listening on port 80");
    }
}

// Embedded Web Dashboard HTML
const char dashboard_html[] = 
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"  <meta charset='UTF-8'>"
"  <meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"  <title>Alzhmind Patient Safety Monitor</title>"
"  <style>"
"    :root { --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --muted: #94a3b8; --danger: #ef4444; --accent: #38bdf8; --success: #10b981; }"
"    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; }"
"    .container { max-width: 900px; margin: 0 auto; }"
"    header { text-align: center; margin-bottom: 25px; }"
"    h1 { margin: 0 0 5px 0; color: var(--accent); }"
"    .alert-banner { display: none; background: var(--danger); color: white; padding: 18px; border-radius: 12px; margin-bottom: 20px; text-align: center; font-size: 20px; font-weight: bold; animation: pulse 1s infinite; }"
"    @keyframes pulse { 0%, 100% { transform: scale(1); } 50% { transform: scale(0.99); opacity: 0.9; } }"
"    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 16px; }"
"    .card { background: var(--card); border-radius: 12px; padding: 20px; border: 1px solid rgba(255,255,255,0.06); }"
"    .card h3 { margin: 0; font-size: 13px; text-transform: uppercase; color: var(--muted); letter-spacing: 0.05em; }"
"    .val { font-size: 34px; font-weight: bold; margin: 10px 0; color: var(--text); }"
"    .unit { font-size: 15px; color: var(--muted); font-weight: normal; }"
"    .sub { font-size: 13px; color: var(--muted); }"
"    .btn { display: inline-block; margin-top: 10px; padding: 8px 16px; border-radius: 8px; text-decoration: none; font-weight: 600; cursor: pointer; border: none; background: var(--accent); color: #000; font-size: 14px; }"
"    .btn-reset { background: #fff; color: #000; margin-top: 12px; }"
"  </style>"
"</head>"
"<body>"
"  <div class='container'>"
"    <header>"
"      <h1>Alzhmind Patient Monitor</h1>"
"      <div class='sub'>ESP-IDF Wearable Safety System</div>"
"    </header>"
"    <div id='fall-box' class='alert-banner'>"
"      &#9888; CRITICAL: PATIENT FALL DETECTED!"
"      <div><button class='btn btn-reset' onclick='resetFall()'>Dismiss / Acknowledge Alarm</button></div>"
"    </div>"
"    <div class='grid'>"
"      <div class='card'>"
"        <h3>Heart Rate (MAX30102)</h3>"
"        <div class='val' id='hr-val'>-- <span class='unit'>BPM</span></div>"
"        <div class='sub' id='finger-sub'>Checking contact...</div>"
"      </div>"
"      <div class='card'>"
"        <h3>Motion & Posture (MPU6050)</h3>"
"        <div class='val' id='motion-val'>1.00 <span class='unit'>G</span></div>"
"        <div class='sub' id='accel-sub'>X: 0.0 | Y: 0.0 | Z: 9.8 m/s&sup2;</div>"
"      </div>"
"      <div class='card'>"
"        <h3>GPS Location (NEO-6M)</h3>"
"        <div class='val' style='font-size:20px;' id='gps-val'>Acquiring Satellites...</div>"
"        <div class='sub' id='gps-sub'>Satellites: 0</div>"
"        <a id='map-link' class='btn' href='#' target='_blank' style='display:none;'>Open in Google Maps</a>"
"      </div>"
"    </div>"
"  </div>"
"  <script>"
"    async function poll() {"
"      try {"
"        const res = await fetch('/api/data');"
"        const d = await res.json();"
"        document.getElementById('fall-box').style.display = d.fall_detected ? 'block' : 'none';"
"        document.getElementById('hr-val').innerHTML = (d.finger_detected && d.heart_rate_bpm > 0) ? (d.heart_rate_bpm + ' <span class=\"unit\">BPM</span>') : ('-- <span class=\"unit\">BPM</span>');"
"        document.getElementById('finger-sub').innerText = d.finger_detected ? 'Sensor Contact Active' : 'No Contact / Finger Removed';"
"        document.getElementById('motion-val').innerHTML = d.acceleration.total_g.toFixed(2) + ' <span class=\"unit\">G</span>';"
"        document.getElementById('accel-sub').innerText = 'X:' + d.acceleration.x.toFixed(1) + ' | Y:' + d.acceleration.y.toFixed(1) + ' | Z:' + d.acceleration.z.toFixed(1) + ' m/s²';"
"        const mapLink = document.getElementById('map-link');"
"        if (d.gps.valid) {"
"          document.getElementById('gps-val').innerText = d.gps.latitude.toFixed(5) + ', ' + d.gps.longitude.toFixed(5);"
"          document.getElementById('gps-sub').innerText = 'Satellites: ' + d.gps.satellites + ' | Speed: ' + d.gps.speed_kmph.toFixed(1) + ' km/h';"
"          mapLink.href = 'https://www.google.com/maps?q=' + d.gps.latitude + ',' + d.gps.longitude;"
"          mapLink.style.display = 'inline-block';"
"        } else {"
"          document.getElementById('gps-val').innerText = 'Acquiring Satellites...';"
"          document.getElementById('gps-sub').innerText = 'Satellites Tracked: ' + d.gps.satellites;"
"          mapLink.style.display = 'none';"
"        }"
"      } catch(e) { console.error(e); }"
"    }"
"    async function resetFall() {"
"      await fetch('/api/reset_fall', {method: 'POST'});"
"      document.getElementById('fall-box').style.display = 'none';"
"    }"
"    setInterval(poll, 1000);"
"    poll();"
"  </script>"
"</body>"
"</html>";
