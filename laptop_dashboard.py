#!/usr/bin/env python3
"""
Alzhmind - Laptop Monitor & Alert Receiver
Continuously fetches patient vitals, GPS coordinates, and fall alerts from the ESP32 over WiFi.
"""

import sys
import time
import requests
from datetime import datetime

# Configure your ESP32 IP address (Printed on Serial Monitor and OLED display)
ESP32_IP = "192.168.4.1"  # Default when connected to ESP32 hotspot, or local router IP
API_URL = f"http://{ESP32_IP}/api/data"

def print_banner():
    print("=" * 65)
    print("        ALZHMIND - PATIENT SAFETY LAPTOP MONITOR")
    print("=" * 65)
    print(f"Target ESP32 Device: {API_URL}")
    print("Press Ctrl+C to stop monitoring.\n")

def monitor_patient(esp_ip):
    url = f"http://{esp_ip}/api/data"
    print(f"Connecting to ESP32 at {url}...")
    
    last_fall_state = False

    while True:
        try:
            response = requests.get(url, timeout=3.0)
            if response.status_code == 200:
                data = response.json()
                timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

                fall_alert = data.get("fall_alert", False)
                bpm = data.get("heart_rate_bpm", 0)
                finger = data.get("finger_detected", False)
                gps_valid = data.get("gps_valid", False)
                lat = data.get("latitude", 0.0)
                lng = data.get("longitude", 0.0)
                sats = data.get("satellites", 0)
                maps_url = data.get("google_maps_url", "")

                if fall_alert:
                    print("\n" + "!" * 65)
                    print(f"[{timestamp}] CRITICAL WARNING: FALL DETECTED FOR PATIENT!")
                    print(f"Location: {lat:.6f}, {lng:.6f} ({sats} satellites)")
                    print(f"Google Maps Link: {maps_url}")
                    print(f"Heart Rate: {bpm} BPM")
                    print("!" * 65 + "\n")
                else:
                    status_line = (
                        f"[{timestamp}] BPM: {bpm:<3} | Finger: {'YES' if finger else 'NO ':<3} | "
                        f"GPS: {'LOCK' if gps_valid else 'SEARCH':<6} ({lat:.4f}, {lng:.4f}) | "
                        f"Status: NORMAL"
                    )
                    print(status_line)

            else:
                print(f"Unexpected status from ESP32: {response.status_code}")

        except requests.exceptions.RequestException as e:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Waiting for ESP32 connection at {url}... ({e})")

        time.sleep(1.0)

if __name__ == "__main__":
    ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    print_banner()
    try:
        monitor_patient(ip)
    except KeyboardInterrupt:
        print("\nMonitoring stopped by user.")
