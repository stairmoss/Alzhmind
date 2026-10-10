#!/usr/bin/env python3
"""Local-first caregiver dashboard for Alzhmind / Memoria.

The dashboard stores a small telemetry and visitor history in SQLite locally.
It can poll the existing ESP-IDF wearable endpoint or accept a LAN webhook at
``POST /api/telemetry``.  Set DASHBOARD_TOKEN to require a Bearer token for
writes; bind to a trusted LAN only in production.
"""
from __future__ import annotations

import json
import os
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import requests
from flask import Flask, Response, abort, jsonify, render_template_string, request

BASE_DIR = Path(__file__).resolve().parent
DATABASE = Path(os.getenv("MEMORIA_DB", BASE_DIR / "memoria.db"))
WEARABLE_URL = os.getenv("WEARABLE_URL", "").rstrip("/")
WRITE_TOKEN = os.getenv("DASHBOARD_TOKEN", "")

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 16 * 1024

PAGE = """<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>
<title>Memoria Care Dashboard</title><style>
body{margin:0;background:#f7f9fb;color:#253342;font:16px system-ui,sans-serif}.wrap{max-width:1050px;margin:auto;padding:56px 24px}h1{font-weight:500;letter-spacing:.02em}.sub{color:#64748b;margin-bottom:36px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:16px}.card{background:#fff;border:1px solid #e5eaf0;border-radius:12px;padding:20px;min-height:86px}.label{color:#64748b;font-size:.83rem;text-transform:uppercase;letter-spacing:.08em}.value{font-size:1.7rem;margin-top:10px}.alert{background:#fff1f2;border-color:#fecdd3;color:#9f1239}.ok{color:#0f766e}a,button{color:#176b9d}button{background:#fff;border:1px solid #b9cbd8;border-radius:7px;padding:9px 12px;cursor:pointer}section{margin-top:38px}table{width:100%;border-collapse:collapse;background:#fff}th,td{text-align:left;padding:12px;border-bottom:1px solid #e5eaf0;font-size:.9rem}@media(max-width:600px){.wrap{padding:28px 14px}table{font-size:.75rem}}</style>
<main class=wrap><h1>Memoria</h1><p class=sub>Local caregiver view · telemetry remains on this device by default.</p>
<div id=notice></div><div class=grid><div class=card><div class=label>Heart rate</div><div id=bpm class=value>—</div></div><div class=card><div class=label>Motion</div><div id=motion class=value>—</div></div><div class=card><div class=label>GPS</div><div id=gps class=value>—</div></div><div class=card><div class=label>Last update</div><div id=updated class=value>—</div></div></div>
<section><button onclick='refreshData()'>Refresh wearable</button> <a id=map target=_blank rel=noopener>Open map</a></section><section><h2>Recent visitor context</h2><table><thead><tr><th>Time</th><th>Name</th><th>Relation</th><th>Context</th></tr></thead><tbody id=visits></tbody></table></section></main>
<script>function esc(s){const n=document.createElement('span');n.textContent=s??'';return n.innerHTML}async function load(){let d=await fetch('/api/live').then(r=>r.json());let t=d.telemetry||{},a=t.acceleration||{},g=t.gps||{};bpm.textContent=t.heart_rate_bpm?`${t.heart_rate_bpm} BPM`:'No reading';motion.textContent=a.total_g?`${a.total_g} G`:'—';gps.textContent=g.valid?`${Number(g.latitude).toFixed(4)}, ${Number(g.longitude).toFixed(4)}`:'No fix';updated.textContent=d.received_at?new Date(d.received_at).toLocaleTimeString():'—';notice.innerHTML=t.fall_detected?'<div class="card alert">Fall alert active — verify the patient’s wellbeing immediately.</div>':'';map.href=g.valid?`https://maps.google.com/?q=${g.latitude},${g.longitude}`:'#';let v=await fetch('/api/visits').then(r=>r.json());visits.innerHTML=v.map(x=>`<tr><td>${esc(new Date(x.created_at).toLocaleString())}</td><td>${esc(x.name)}</td><td>${esc(x.relation)}</td><td>${esc(x.context_note)}</td></tr>`).join('')||'<tr><td colspan=4>No local visitor records.</td></tr>'}async function refreshData(){await fetch('/api/refresh',{method:'POST'});load()}load();setInterval(load,5000)</script></html>"""


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def connection() -> sqlite3.Connection:
    db = sqlite3.connect(DATABASE)
    db.row_factory = sqlite3.Row
    return db


def init_db() -> None:
    with connection() as db:
        db.executescript("""CREATE TABLE IF NOT EXISTS telemetry (id INTEGER PRIMARY KEY, received_at TEXT NOT NULL, payload TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS visits (id INTEGER PRIMARY KEY, created_at TEXT NOT NULL, name TEXT NOT NULL, relation TEXT NOT NULL, context_note TEXT NOT NULL);""")


def authorize_write() -> None:
    if WRITE_TOKEN and request.headers.get("Authorization") != f"Bearer {WRITE_TOKEN}":
        abort(401)


def validate_payload(data: Any) -> dict[str, Any]:
    if not isinstance(data, dict) or not isinstance(data.get("device_id", "unknown"), str):
        abort(400, "Expected wearable telemetry JSON object")
    return data


def save_telemetry(data: dict[str, Any]) -> None:
    with connection() as db:
        db.execute("INSERT INTO telemetry(received_at,payload) VALUES (?,?)", (now(), json.dumps(data, separators=(",", ":"))))


def latest() -> tuple[dict[str, Any] | None, str | None]:
    with connection() as db:
        row = db.execute("SELECT received_at,payload FROM telemetry ORDER BY id DESC LIMIT 1").fetchone()
    return (json.loads(row["payload"]), row["received_at"]) if row else (None, None)


@app.get("/")
def dashboard() -> str:
    return render_template_string(PAGE)


@app.get("/api/live")
def live() -> Response:
    data, received = latest()
    return jsonify({"telemetry": data, "received_at": received})


@app.post("/api/telemetry")
def ingest_telemetry() -> Response:
    authorize_write()
    data = validate_payload(request.get_json(silent=True))
    save_telemetry(data)
    return jsonify({"ok": True}), 201


@app.post("/api/refresh")
def refresh_wearable() -> Response:
    if not WEARABLE_URL:
        return jsonify({"ok": False, "error": "Set WEARABLE_URL to the ESP32 base URL."}), 409
    try:
        response = requests.get(f"{WEARABLE_URL}/api/data", timeout=3)
        response.raise_for_status()
        save_telemetry(validate_payload(response.json()))
    except requests.RequestException as exc:
        return jsonify({"ok": False, "error": str(exc)}), 502
    return jsonify({"ok": True})


@app.get("/api/visits")
def visits() -> Response:
    with connection() as db:
        rows = db.execute("SELECT created_at,name,relation,context_note FROM visits ORDER BY id DESC LIMIT 50").fetchall()
    return jsonify([dict(row) for row in rows])


@app.post("/api/visits")
def log_visit() -> Response:
    authorize_write()
    data = request.get_json(silent=True) or {}
    fields = [str(data.get(key, "")).strip()[:200] for key in ("name", "relation", "context_note")]
    if not all(fields):
        abort(400, "name, relation, and context_note are required")
    with connection() as db:
        db.execute("INSERT INTO visits(created_at,name,relation,context_note) VALUES(?,?,?,?)", (now(), *fields))
    return jsonify({"ok": True}), 201


init_db()
if __name__ == "__main__":
    app.run(host=os.getenv("DASHBOARD_HOST", "127.0.0.1"), port=int(os.getenv("DASHBOARD_PORT", "5000")), debug=False)
