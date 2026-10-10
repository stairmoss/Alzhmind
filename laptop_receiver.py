#!/usr/bin/env python3
"""
Alzhmind Patient Monitor - Laptop Companion Script
Fetches real-time telemetry from the Alzhmind ESP32 device over WiFi.
Displays live patient stats, logs data to CSV, and triggers audio/visual alarms on fall detection.
"""

import sys
import time
import json
import csv
import os
from datetime import datetime

try:
    import requests
except ImportError:
    print("[ERROR] 'requests' library not found. Install via: pip install requests")
    sys.exit(1)

# ESP32 Device Configuration
# Update this with your ESP32's IP address (displayed on ESP32 Serial Monitor / OLED)
ESP32_IP = "192.168.4.1"  # Default when connected to Alzhmind-Wearable SoftAP
# If connected to home router, e.g.: ESP32_IP = "192.168.1.105"
API_URL = f"http://{ESP32_IP}/api/data"
RESET_URL = f"http://{ESP32_IP}/api/reset_fall"
POLL_INTERVAL_SEC = 1.0

CSV_FILE = "alzhmind_patient_logs.csv"

def init_csv():
    if not os.path.exists(CSV_FILE):
        with open(CSV_FILE, mode='w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                "Timestamp", "Device_ID", "Fall_Detected", "Heart_Rate_BPM",
                "Accel_Total_G", "Accel_X", "Accel_Y", "Accel_Z",
                "GPS_Valid", "Latitude", "Longitude", "Satellites", "Speed_kmph"
            ])

def log_to_csv(data):
    try:
        with open(CSV_FILE, mode='a', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                data.get("device_id", "N/A"),
                data.get("fall_detected", False),
                data.get("heart_rate_bpm", 0),
                data.get("acceleration", {}).get("total_g", 0.0),
                data.get("acceleration", {}).get("x", 0.0),
                data.get("acceleration", {}).get("y", 0.0),
                data.get("acceleration", {}).get("z", 0.0),
                data.get("gps", {}).get("valid", False),
                data.get("gps", {}).get("latitude", 0.0),
                data.get("gps", {}).get("longitude", 0.0),
                data.get("gps", {}).get("satellites", 0),
                data.get("gps", {}).get("speed_kmph", 0.0)
            ])
    except Exception as e:
        print(f"[WARN] Error writing to CSV: {e}")

def main():
    print("=" * 60)
    print("   Alzhmind Patient Safety Monitor - Laptop Client   ")
    print(f"   Target Device: {API_URL}")
    print(f"   Logging to: {CSV_FILE}")
    print("=" * 60)

    init_csv()

    while True:
        try:
            resp = requests.get(API_URL, timeout=3.0)
            if resp.status_code == 200:
                data = resp.json()
                fall = data.get("fall_detected", False)
                hr = data.get("heart_rate_bpm", 0)
                accel_g = data.get("acceleration", {}).get("total_g", 0.0)
                gps = data.get("gps", {})

                # Terminal Output
                timestamp = datetime.now().strftime("%H:%M:%S")
                if fall:
                    print(f"\n\033[91;1m[!ALERT!] [{timestamp}] CRITICAL: PATIENT FALL DETECTED!\033[0m")
                    print(f"Impact: {accel_g:.2f} G | Heart Rate: {hr} BPM")
                    if gps.get("valid"):
                        print(f"Location: https://www.google.com/maps?q={gps.get('latitude')},{gps.get('longitude')}")
                    # Bell sound on terminal
                    print("\a", end="", flush=True)
                else:
                    gps_status = f"Lat: {gps.get('latitude',0):.4f}, Lng: {gps.get('longitude',0):.4f} (Sats: {gps.get('satellites',0)})" if gps.get("valid") else f"GPS Searching (Sats: {gps.get('satellites', 0)})"
                    print(f"[{timestamp}] Pulse: {hr:3d} BPM | Motion: {accel_g:.2f} G | {gps_status}")

                log_to_csv(data)
            else:
                print(f"[WARN] Unexpected status code: {resp.status_code}")

        except requests.exceptions.RequestException:
            print(f"[DISCONNECTED] Searching for ESP32 at {API_URL}...", end="\r")

        time.sleep(POLL_INTERVAL_SEC)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[INFO] Monitor stopped by caregiver.")

