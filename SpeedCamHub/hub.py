import os
import time
import datetime as dt
from pathlib import Path

import requests
from flask import Flask, request, jsonify

import cv2
import numpy as np

from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build
from googleapiclient.http import MediaFileUpload

from lpr import LPR

# ---------------- USER CONFIG ----------------
PHONE_SNAPSHOT_URL = "http://192.168.10.3:8080/shot.jpg"
DRIVE_FOLDER_ID    = "1GbfZkWk5w4D3Y57F6D8WEu2BpWMB9uoo"
SPREADSHEET_ID     = "1ArVmbO4Yu6X-ebZarwCsTGF5_e4vFMylG0_amqfneBA"

SHEET_TAB_NAME     = "Sheet1"
APPEND_RANGE       = f"{SHEET_TAB_NAME}!A1"

CAPTURE_DIR        = Path("captures")
COOLDOWN_SECONDS   = 2.0

MODEL_PATH         = str(Path("models") / "best.pt")  # put best.pt here
# --------------------------------------------

SCOPES = [
    "https://www.googleapis.com/auth/spreadsheets",
    "https://www.googleapis.com/auth/drive.file",
]

app = Flask(__name__)
CAPTURE_DIR.mkdir(exist_ok=True)

_last_trigger_ts = 0.0

_drive = None
_sheets = None
_lpr = None

def get_google_services():
    creds = None
    token_path = Path("token.json")
    cred_path  = Path("credentials.json")

    if token_path.exists():
        creds = Credentials.from_authorized_user_file(str(token_path), SCOPES)

    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            creds.refresh(Request())
        else:
            if not cred_path.exists():
                raise FileNotFoundError("credentials.json not found. Put your OAuth JSON here and rename it to credentials.json")
            flow = InstalledAppFlow.from_client_secrets_file(str(cred_path), SCOPES)
            creds = flow.run_local_server(port=0)
        token_path.write_text(creds.to_json(), encoding="utf-8")

    drive = build("drive", "v3", credentials=creds)
    sheets = build("sheets", "v4", credentials=creds)
    return drive, sheets

def init_once():
    global _drive, _sheets, _lpr
    if _drive is None or _sheets is None:
        _drive, _sheets = get_google_services()
    if _lpr is None:
        if not Path(MODEL_PATH).exists():
            raise FileNotFoundError(f"Model not found at {MODEL_PATH}. Put your YOLO plate model best.pt there.")
        _lpr = LPR(MODEL_PATH)

def capture_snapshot(speed_kmh: float) -> Path:
    ts = dt.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    fname = f"{ts}_speed_{speed_kmh:.1f}.jpg"
    out_path = CAPTURE_DIR / fname

    r = requests.get(PHONE_SNAPSHOT_URL, timeout=5)
    r.raise_for_status()
    out_path.write_bytes(r.content)
    return out_path

def upload_to_drive(drive_service, file_path: Path, mimetype="image/jpeg") -> str:
    file_metadata = {"name": file_path.name, "parents": [DRIVE_FOLDER_ID]}
    media = MediaFileUpload(str(file_path), mimetype=mimetype, resumable=False)

    created = drive_service.files().create(
        body=file_metadata,
        media_body=media,
        fields="id, webViewLink"
    ).execute()

    file_id = created["id"]
    web_view = created.get("webViewLink")
    if not web_view:
        web_view = drive_service.files().get(fileId=file_id, fields="webViewLink").execute().get("webViewLink", "")

    return web_view or f"https://drive.google.com/file/d/{file_id}/view"

def append_to_sheet(sheets_service, row_values):
    # Uses Sheets "values.append" to append to next row. :contentReference[oaicite:10]{index=10}
    body = {"values": [row_values]}
    sheets_service.spreadsheets().values().append(
        spreadsheetId=SPREADSHEET_ID,
        range=APPEND_RANGE,
        valueInputOption="USER_ENTERED",
        insertDataOption="INSERT_ROWS",
        body=body
    ).execute()

@app.get("/trigger")
def trigger():
    global _last_trigger_ts
    now = time.time()
    if now - _last_trigger_ts < COOLDOWN_SECONDS:
        return jsonify({"ok": False, "error": "cooldown"}), 429
    _last_trigger_ts = now

    speed = request.args.get("speed", type=float, default=None)
    if speed is None:
        return jsonify({"ok": False, "error": "missing speed query param"}), 400

    try:
        init_once()

        # 1) Capture
        img_path = capture_snapshot(speed)

        # 2) AI: detect plate + OCR
        img = cv2.imread(str(img_path))
        plate_text, det_conf, ocr_conf, crop = _lpr.detect_and_read(img)

        crop_path = None
        crop_link = ""
        if crop is not None:
            crop_path = img_path.with_name(img_path.stem + "_plate.jpg")
            cv2.imwrite(str(crop_path), crop)

        # 3) Upload to Drive (full + crop)
        full_link = upload_to_drive(_drive, img_path)
        if crop_path is not None and crop_path.exists():
            crop_link = upload_to_drive(_drive, crop_path)

        # 4) Append to Sheet
        timestamp_str = dt.datetime.now().isoformat(timespec="seconds")
        overspeed_str = "YES"

        row = [
            timestamp_str,
            f"{speed:.2f}",
            overspeed_str,
            full_link,
            plate_text,
            f"{det_conf:.3f}",
            f"{ocr_conf:.3f}",
            crop_link
        ]
        append_to_sheet(_sheets, row)

        return jsonify({
            "ok": True,
            "speed": speed,
            "image": full_link,
            "plate_text": plate_text,
            "det_conf": det_conf,
            "ocr_conf": ocr_conf,
            "plate_crop": crop_link,
            "file": img_path.name
        })

    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500

if __name__ == "__main__":
    # Listen on all interfaces so ESP32 can reach it
    app.run(host="0.0.0.0", port=5000, debug=False)
