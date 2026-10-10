#!/usr/bin/env python3
"""Opt-in, local-first social-context companion.

Known face encodings and profiles stay in ``data/``.  Nothing is uploaded by
default.  The optional AI fallback only sends the explicit task description
when HACKCLUB_API_KEY is configured.  Press q to quit, n to register the
largest currently visible face (after completing the terminal prompts).
"""
from __future__ import annotations

import argparse
import json
import os
import sqlite3
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import cv2
    import face_recognition
    import pyttsx3
except ImportError as exc:  # keeps --help usable without heavy optional packages
    raise SystemExit("Install optional vision dependencies: pip install opencv-python face-recognition pyttsx3") from exc

DATA_DIR = Path(os.getenv("MEMORIA_DATA_DIR", Path(__file__).parent / "data"))
PROFILES = DATA_DIR / "profiles.json"
VISITS_DB = DATA_DIR / "visits.db"
COOLDOWN_SECONDS = 180


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def load_profiles() -> list[dict[str, Any]]:
    DATA_DIR.mkdir(mode=0o700, exist_ok=True)
    if not PROFILES.exists():
        PROFILES.write_text("[]\n", encoding="utf-8")
        try:
            PROFILES.chmod(0o600)
        except OSError:
            pass
    try:
        result = json.loads(PROFILES.read_text(encoding="utf-8"))
        return result if isinstance(result, list) else []
    except json.JSONDecodeError:
        raise SystemExit(f"Profile file is invalid JSON: {PROFILES}")


def save_profiles(profiles: list[dict[str, Any]]) -> None:
    temp = PROFILES.with_suffix(".tmp")
    temp.write_text(json.dumps(profiles, indent=2), encoding="utf-8")
    temp.replace(PROFILES)


def log_visit(profile: dict[str, Any]) -> None:
    with sqlite3.connect(VISITS_DB) as db:
        db.execute("CREATE TABLE IF NOT EXISTS visits(created_at TEXT, name TEXT, relation TEXT, context_note TEXT)")
        db.execute("INSERT INTO visits VALUES(?,?,?,?)", (utc_now(), profile["name"], profile["relation"], profile.get("context_note", "")))


def speak(engine: Any, phrase: str) -> None:
    engine.say(phrase)
    engine.runAndWait()


def gentle_prompt(task_context: str) -> str:
    """Use the configured proxy only for an explicit, non-identifying task cue."""
    api_key = os.getenv("HACKCLUB_API_KEY")
    if not api_key:
        return "Would you like a gentle reminder about what you were doing?"
    try:
        from openai import OpenAI
        client = OpenAI(base_url="https://ai.hackclub.com/proxy/v1", api_key=api_key)
        response = client.chat.completions.create(model="openai/gpt-4o-mini", messages=[
            {"role": "system", "content": "Write one warm, non-commanding, dignity-preserving reminder in 20 words or fewer. Do not claim certainty or diagnose."},
            {"role": "user", "content": f"Possible unfinished task: {task_context}"},
        ], max_tokens=60, temperature=0.4)
        return response.choices[0].message.content.strip()
    except Exception:
        return "Would you like a gentle reminder about what you were doing?"


def register_face(frame: Any, location: tuple[int, int, int, int], profiles: list[dict[str, Any]]) -> None:
    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    encodings = face_recognition.face_encodings(rgb, [location])
    if not encodings:
        print("Could not encode that face; try better lighting.")
        return
    name = input("Name: ").strip()
    relation = input("Relationship: ").strip()
    context = input("Helpful context note (optional): ").strip()
    hook = input("Conversation hook (optional): ").strip()
    if not name or not relation:
        print("Registration cancelled: name and relationship are required.")
        return
    profiles.append({"name": name[:100], "relation": relation[:100], "context_note": context[:300], "conversation_hook": hook[:300], "encoding": encodings[0].tolist()})
    save_profiles(profiles)
    print(f"Registered {name} locally.")


def run(camera_index: int, task_context: str, pause_seconds: int) -> None:
    profiles = load_profiles()
    engine = pyttsx3.init()
    camera = cv2.VideoCapture(camera_index)
    if not camera.isOpened():
        raise SystemExit(f"Cannot open camera {camera_index}")
    last_seen: dict[str, float] = {}
    last_motion = time.monotonic()
    prior_gray = None
    print("Camera active. Press q to exit; n registers the largest visible face.")
    try:
        while True:
            ok, frame = camera.read()
            if not ok:
                break
            small = cv2.resize(frame, (0, 0), fx=0.25, fy=0.25)
            rgb = cv2.cvtColor(small, cv2.COLOR_BGR2RGB)
            locations = face_recognition.face_locations(rgb)
            encodings = face_recognition.face_encodings(rgb, locations)
            known = [item["encoding"] for item in profiles]
            for encoding, (top, right, bottom, left) in zip(encodings, locations):
                profile = None
                if known:
                    distances = face_recognition.face_distance(known, encoding)
                    index = int(distances.argmin())
                    if distances[index] < 0.48:
                        profile = profiles[index]
                color, label = ((40, 150, 70), profile["name"]) if profile else ((30, 30, 220), "Unknown")
                cv2.rectangle(frame, (left * 4, top * 4), (right * 4, bottom * 4), color, 2)
                cv2.putText(frame, label, (left * 4, max(25, top * 4 - 8)), cv2.FONT_HERSHEY_SIMPLEX, .7, color, 2)
                if profile and time.monotonic() - last_seen.get(profile["name"], 0) > COOLDOWN_SECONDS:
                    phrase = f"Hello. It's your {profile['relation']} {profile['name']} here."
                    speak(engine, phrase)
                    log_visit(profile)
                    last_seen[profile["name"]] = time.monotonic()
            gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
            if prior_gray is not None and cv2.absdiff(gray, prior_gray).mean() > 3.0:
                last_motion = time.monotonic()
            prior_gray = gray
            if task_context and time.monotonic() - last_motion > pause_seconds:
                speak(engine, gentle_prompt(task_context))
                last_motion = time.monotonic()
            cv2.imshow("Memoria local companion", frame)
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            if key == ord("n") and locations:
                largest = max(locations, key=lambda box: (box[2] - box[0]) * (box[1] - box[3]))
                register_face(frame, tuple(x * 4 for x in largest), profiles)
    finally:
        camera.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--camera", type=int, default=0)
    parser.add_argument("--task-context", default="", help="Explicit, non-identifying task cue for a paused-task reminder")
    parser.add_argument("--pause-seconds", type=int, default=45)
    args = parser.parse_args()
    run(args.camera, args.task_context, args.pause_seconds)
