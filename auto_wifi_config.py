#!/usr/bin/env python3
"""Discover an Alzhmind wearable on the current IPv4 LAN.

The scanner only probes the documented, read-only ``/api/data`` endpoint.  It
does not attempt authentication, configuration, or access outside the local
network.  Use ``--host`` to skip discovery when the IP is already known.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import ipaddress
import json
import socket
from typing import Any

import requests

DEFAULT_PORT = 80
ENDPOINT = "/api/data"


def local_network() -> ipaddress.IPv4Network:
    """Return the most likely connected IPv4 /24 without sending packets."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("8.8.8.8", 80))
        address = probe.getsockname()[0]
    finally:
        probe.close()
    return ipaddress.ip_network(f"{address}/24", strict=False)


def telemetry_url(host: str, port: int = DEFAULT_PORT) -> str:
    return f"http://{host}:{port}{ENDPOINT}"


def probe(host: str, port: int, timeout: float) -> tuple[str, dict[str, Any]] | None:
    try:
        response = requests.get(telemetry_url(host, port), timeout=timeout)
        payload = response.json()
        if response.ok and isinstance(payload, dict) and (
            "device_id" in payload or "heart_rate_bpm" in payload
        ):
            return host, payload
    except (requests.RequestException, ValueError):
        pass
    return None


def discover(port: int = DEFAULT_PORT, timeout: float = 0.35, workers: int = 48) -> list[tuple[str, dict[str, Any]]]:
    """Probe hosts on the current /24 concurrently and return valid wearables."""
    network = local_network()
    hosts = (str(host) for host in network.hosts())
    found: list[tuple[str, dict[str, Any]]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = (pool.submit(probe, host, port, timeout) for host in hosts)
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            if result:
                found.append(result)
    return sorted(found)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="Known ESP32 IPv4 address; do not scan")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=0.35)
    parser.add_argument("--json", action="store_true", help="Print machine-readable results")
    args = parser.parse_args()
    results = [probe(args.host, args.port, args.timeout)] if args.host else discover(args.port, args.timeout)
    results = [item for item in results if item]
    if args.json:
        print(json.dumps([{"host": host, "url": telemetry_url(host, args.port), "telemetry": data} for host, data in results], indent=2))
    elif results:
        for host, data in results:
            print(f"Found {data.get('device_id', 'Alzhmind wearable')} at {telemetry_url(host, args.port)}")
    else:
        print("No Alzhmind wearable found. Connect to its Wi-Fi AP or pass --host.")
    return 0 if results else 1


if __name__ == "__main__":
    raise SystemExit(main())
