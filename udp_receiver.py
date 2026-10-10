#!/usr/bin/env python3
"""
Alzhmind - Universal UDP Telemetry Receiver
Listens for real-time broadcasts on UDP port 5005 from the ESP32 over Wi-Fi.
No IP configuration required - works automatically on any random device connected to the network.
Displays: Heart Rate, 3-Axis Motion, Total G-Force, Full GPS Lat/Lng with Google Maps URL, and Fall Alerts.
"""

import socket
import json
import csv
import os
from datetime import datetime

UDP_PORT = 5005
BUFFER_SIZE = 1024
CSV_FILE = "alzhmind_live_telemetry.csv"

def init_csv():
    if not os.path.exists(CSV_FILE):
        with open(CSV_FILE, mode='w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                "Timestamp", "Device_ID", "Fall_Detected", "Heart_Rate_BPM", "Finger_Detected",
                "Accel_Total_G", "Accel_X", "Accel_Y", "Accel_Z",
                "GPS_Valid", "Latitude", "Longitude", "Satellites", "Speed_kmph", "Google_Maps_URL"
            ])

def log_to_csv(data):
    try:
        accel = data.get("acceleration", {})
        gps = data.get("gps", {})
        with open(CSV_FILE, mode='a', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                data.get("device_id", "ALZHMIND-PATIENT-01"),
                data.get("fall_detected", False),
                data.get("heart_rate_bpm", 0),
                data.get("finger_detected", False),
                accel.get("total_g", 0.0),
                accel.get("x", 0.0),
                accel.get("y", 0.0),
                accel.get("z", 0.0),
                gps.get("valid", False),
                gps.get("latitude", 0.0),
                gps.get("longitude", 0.0),
                gps.get("satellites", 0),
                gps.get("speed_kmph", 0.0),
                data.get("google_maps_url", "")
            ])
    except Exception as e:
        print(f"[WARN] CSV write error: {e}")

def main():
    print("=" * 75)
    print("    ALZHMIND: ZERO-CONFIG UDP WIRELESS TELEMETRY RECEIVER")
    print(f"    Listening for broadcasts on UDP Port: {UDP_PORT}")
    print(f"    Logging all packets to: {CSV_FILE}")
    print("=" * 75)

    init_csv()

    # Create UDP Socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    # Try enabling SO_REUSEPORT if available on OS
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except Exception:
            pass

    sock.bind(("", UDP_PORT))
    print(f"[READY] Waiting for telemetry stream from ESP32...\n")

    while True:
        try:
            data_bytes, sender_addr = sock.recvfrom(BUFFER_SIZE)
            raw_text = data_bytes.decode('utf-8', errors='ignore').strip()
            
            try:
                data = json.loads(raw_text)
            except json.JSONDecodeError:
                print(f"[RAW-RECV] From {sender_addr[0]}: {raw_text}")
                continue

            ts = datetime.now().strftime("%H:%M:%S")
            fall = data.get("fall_detected", False)
            hr = data.get("heart_rate_bpm", 0)
            finger = data.get("finger_detected", False)
            accel = data.get("acceleration", {})
            total_g = accel.get("total_g", 0.0)
            gps = data.get("gps", {})
            lat = gps.get("latitude", 0.0)
            lng = gps.get("longitude", 0.0)
            sats = gps.get("satellites", 0)
            gps_valid = gps.get("valid", False)
            maps_url = data.get("google_maps_url", f"https://maps.google.com/?q={lat:.6f},{lng:.6f}")

            # Fall Alert Output
            if fall:
                print("\n" + "!" * 75)
                print(f"\033[91;1m[!CRITICAL ALERT!] [{ts}] PATIENT FALL DETECTED (>=10G)!\033[0m")
                print(f"  Sender ESP32 IP : {sender_addr[0]}")
                print(f"  Impact G-Force  : {total_g:.2f} G (X:{accel.get('x',0):.1f}, Y:{accel.get('y',0):.1f}, Z:{accel.get('z',0):.1f} m/s²)")
                print(f"  Heart Rate      : {hr} BPM")
                print(f"  Exact GPS Coords: Latitude = {lat:.6f}, Longitude = {lng:.6f} (Sats: {sats})")
                print(f"  Google Maps Link: {maps_url}")
                print("!" * 75 + "\n")
                print("\a", end="", flush=True)  # Terminal beep
            else:
                gps_str = f"GPS: {lat:.6f}, {lng:.6f} (Sats:{sats})" if gps_valid else f"GPS: Searching ({sats} Sats)"
                pulse_str = f"{hr} BPM" if (finger and hr > 0) else ("Reading..." if finger else "No Contact")
                print(f"[{ts}] From {sender_addr[0]}: Pulse: {pulse_str:<12} | Motion: {total_g:5.2f} G | {gps_str}")

            log_to_csv(data)

        except KeyboardInterrupt:
            print("\n[STOPPED] Telemetry receiver stopped by user.")
            break
        except Exception as e:
            print(f"[ERROR] {e}")

if __name__ == "__main__":
    main()

