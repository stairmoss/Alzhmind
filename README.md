# Memoria / Alzhmind Assistive Ecosystem

This repository now combines the existing **ESP-IDF wearable** with an
optional, local-first companion stack: safe LAN discovery, a Flask caregiver
dashboard, and opt-in on-device face-context cues. It is an assistive prototype,
not a medical device or an emergency-response substitute. Validate thresholds,
power reliability, privacy consent, and caregiver escalation procedures before
any real-world use.

## Quick start (laptop)

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
python auto_wifi_config.py              # finds GET /api/data on the LAN
WEARABLE_URL=http://192.168.4.1 python app.py
```

Open `http://127.0.0.1:5000`. Set `DASHBOARD_HOST=0.0.0.0` only on a trusted
LAN, and set a long random `DASHBOARD_TOKEN` before accepting `POST` writes.
The dashboard writes telemetry and visit records to local SQLite (`memoria.db`).

For opt-in vision support, install the commented optional packages in
`requirements.txt`, obtain informed consent from everyone enrolled, and run:

```bash
python vision_companion.py --task-context "making tea"
```

Known face encodings are stored only in `data/profiles.json`; press `n` while a
face is visible to enroll it, and `q` to stop. The program speaks a social cue
once per visitor per three minutes and logs the local visit. A paused-motion
prompt starts after 45 seconds only if `--task-context` was deliberately given.
Set `HACKCLUB_API_KEY` to enable the optional Hack Club proxy fallback; otherwise
it uses a fully local, generic prompt. Do not send face images or identity data
to the proxy.

## API contract

The dashboard reads the existing ESP-IDF endpoint (`GET /api/data`) or accepts
the same JSON shape at `POST /api/telemetry`. Its public read endpoints are:

- `GET /api/live` — latest locally retained telemetry.
- `GET /api/visits` — last 50 local visitor-context entries.
- `POST /api/refresh` — polls `WEARABLE_URL/api/data`.

With `DASHBOARD_TOKEN` set, write endpoints require
`Authorization: Bearer <token>`. Keep the dashboard behind an authenticated
reverse proxy/VPN for caregiver access; the included “video check-in” surface is
intentionally not a video service because a real deployment needs authenticated,
audited WebRTC or a compliant provider.

## Firmware choices

`main/` remains the production ESP-IDF implementation. It shares I2C GPIO
21/22, GPS UART2 GPIO 16/17, and exposes `/api/data`. The removed hard-coded
household Wi-Fi values have been replaced with empty build-time defaults; set
credentials through deployment configuration and replace the default AP password
before use. `esp32_firmware.ino` is a separate Arduino reference requested for
Arduino IDE users. Copy `secrets.h.example` to `secrets.h`, install TinyGPSPlus,
and set a unique AP password before flashing. It implements impact-plus-stillness
fall detection, a 30-second local grace alarm, acknowledgement button GPIO 27,
and the same telemetry schema. Its MAX30102 hook intentionally reports no pulse
until you integrate and validate a sensor library.

## Privacy and safety boundaries

- Get meaningful consent, provide a visible off switch, and document who can
  see data. Do not use this for covert monitoring.
- Face encodings, telemetry, and logs are sensitive personal data: encrypt disks,
  restrict OS accounts, set retention limits, and back up only with consent.
- Firebase synchronization is not enabled by default. If added, use least-
  privilege service credentials, server-side rules, and send only the minimum
  consented fields. Never put Firebase keys or the Hack Club API key in source.
- Test falls with a test rig, not a person; tune per wearer and always retain a
  manual emergency path. A non-acknowledged alarm is a caregiver notification,
  not proof of a fall.

---

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
