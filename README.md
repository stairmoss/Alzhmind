# Alzhmind: Native ESP-IDF (C) Wearable Patient Safety System

A native **ESP-IDF (Pure C)** firmware for the ESP32 designed to safeguard Alzheimer's patients. The device monitors vital signs (heart rate), tracks real-time GPS coordinates, detects falls via 6-axis inertial motion, triggers localized audio/visual alarms, and continuously transmits live telemetry to a caregiver's laptop over Wi-Fi.

---

## 1. Project Structure

```
/mnt/18A660FBA660DB30/Alzhmind/
├── CMakeLists.txt                 # Project CMake configuration
├── sdkconfig.defaults             # Default ESP-IDF target and settings
├── main/
│   ├── CMakeLists.txt             # Component registration
│   ├── main.c                     # System tasks, Wi-Fi, HTTP server & state management
│   ├── ssd1306.h / ssd1306.c      # Native I2C driver & graphics library for 128x64 OLED
│   ├── mpu6050.h / mpu6050.c      # Native I2C driver & fall detection logic
│   ├── max30102.h / max30102.c    # Native I2C driver & heart rate DSP algorithm
│   └── gps_nmea.h / gps_nmea.c    # Native UART2 NMEA parser for NEO-6M GPS
├── laptop_receiver.py             # Python telemetry receiver & CSV logger for laptop
└── README.md
```

---

## 2. Hardware Wiring & Pinout Mapping

| Component | Signal / Pin | ESP32 GPIO | Description |
| :--- | :--- | :--- | :--- |
| **Shared I2C Bus** | | | *Internal pull-up enabled on I2C_NUM_0* |
| SSD1306 OLED (0x3C) | SDA | **GPIO 21** | I2C Data Line |
| SSD1306 OLED (0x3C) | SCL | **GPIO 22** | I2C Clock Line |
| MAX30102 / HW-605 (0x57) | SDA | **GPIO 21** | I2C Data Line |
| MAX30102 / HW-605 (0x57) | SCL | **GPIO 22** | I2C Clock Line |
| MPU6050 IMU (0x68) | SDA | **GPIO 21** | I2C Data Line |
| MPU6050 IMU (0x68) | SCL | **GPIO 22** | I2C Clock Line |
| **NEO-6M GPS Module** | | | *Hardware UART_NUM_2 at 9600 baud* |
| NEO-6M GPS | TX | **GPIO 16** | Connects to ESP32 RX2 |
| NEO-6M GPS | RX | **GPIO 17** | Connects to ESP32 TX2 |
| **Emergency Indicators** | | | |
| Active Buzzer | Signal (+) | **GPIO 12** | Active-High audible alarm |
| Alert LED | Anode (+) | **GPIO 14** | Active-High strobe alarm (via 220Ω resistor) |
| Power Rails | VCC / GND | 3.3V / 5V / GND | OLED (3.3V), MPU6050 (3.3V), MAX30102 (3.3V), GPS (3.3V/5V) |

---

## 3. How to Build & Flash with ESP-IDF

### Step 1: Set up ESP-IDF Environment
```bash
. $HOME/esp/esp-idf/export.sh
```

### Step 2: Build the Project
```bash
cd /mnt/18A660FBA660DB30/Alzhmind
idf.py build
```

### Step 3: Flash to ESP32 & Monitor
```bash
idf.py -p /dev/ttyUSB0 flash monitor
# (Replace /dev/ttyUSB0 with your serial port, e.g. /dev/ttyACM0)
```

---

## 4. Laptop Monitoring Over Wi-Fi

The device runs in **Dual Wi-Fi Mode (AP + STA)**:
- **Station Mode**: Connects to your local Wi-Fi router (configured in `main.c` via `WIFI_STA_SSID` and `WIFI_STA_PASS`).
- **SoftAP Mode (Standalone)**: If no Wi-Fi router is available, it broadcasts its own network:
  - **SSID:** `Alzhmind-Wearable`
  - **Password:** `alzhmind123`
  - **Device IP:** `192.168.4.1`

### A. Web Browser Live Dashboard
Open any browser on your laptop:
```
http://<ESP32_IP>/
# e.g., http://192.168.4.1/ or http://192.168.1.xxx/
```
Features:
- Live pulse reading with contact detection.
- 3-axis motion and total G-force.
- Real-time GPS location with one-click **"Open in Google Maps"**.
- Emergency flashing banner on **Fall Detection** with an interactive **"Dismiss Alarm"** button.

### B. REST API JSON Telemetry
Any script or software can poll:
```http
GET http://<ESP32_IP>/api/data
```
**JSON Response:**
```json
{
  "device_id": "ALZHMIND-PATIENT-01",
  "fall_detected": false,
  "heart_rate_bpm": 72,
  "finger_detected": true,
  "acceleration": {
    "x": 0.08,
    "y": -0.04,
    "z": 9.81,
    "total_g": 1.00
  },
  "gps": {
    "valid": true,
    "latitude": 12.971598,
    "longitude": 77.594562,
    "speed_kmph": 0.0,
    "satellites": 7
  },
  "uptime_sec": 48
}
```

### C. Python Companion Client
Run the terminal client on your laptop:
```bash
python3 laptop_receiver.py
```
Outputs live status to terminal, sounds a beep on patient fall, and writes timestamped logs to `alzhmind_patient_logs.csv`.
