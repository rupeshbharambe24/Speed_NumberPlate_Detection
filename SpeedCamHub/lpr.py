# lpr.py
import re
import cv2
import numpy as np
import easyocr
from ultralytics import YOLO

ALLOWED = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

class LPR:
    def __init__(self, weights_path: str):
        # Load once for speed
        self.detector = YOLO(weights_path)  # :contentReference[oaicite:5]{index=5}
        self.reader = easyocr.Reader(['en'], gpu=False)  # :contentReference[oaicite:6]{index=6}

    def _clean(self, s: str) -> str:
        s = s.upper()
        s = re.sub(r'[^A-Z0-9]', '', s)
        return s

    def detect_and_read(self, image_bgr: np.ndarray):
        """
        Returns: (plate_text, det_conf, ocr_conf, crop_bgr)
        """
        results = self.detector.predict(image_bgr, imgsz=640, conf=0.25, verbose=False)  # :contentReference[oaicite:7]{index=7}
        r0 = results[0]
        if r0.boxes is None or len(r0.boxes) == 0:
            return "", 0.0, 0.0, None

        confs = r0.boxes.conf.cpu().numpy()
        xyxy  = r0.boxes.xyxy.cpu().numpy()  # :contentReference[oaicite:8]{index=8}
        idx = int(np.argmax(confs))
        det_conf = float(confs[idx])
        x1, y1, x2, y2 = xyxy[idx].astype(int)

        h, w = image_bgr.shape[:2]
        pad_x = int(0.08 * (x2 - x1))
        pad_y = int(0.15 * (y2 - y1))
        x1 = max(0, x1 - pad_x); y1 = max(0, y1 - pad_y)
        x2 = min(w - 1, x2 + pad_x); y2 = min(h - 1, y2 + pad_y)

        crop = image_bgr[y1:y2, x1:x2].copy()
        if crop.size == 0:
            return "", det_conf, 0.0, None

        # OCR preprocessing
        gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
        gray = cv2.bilateralFilter(gray, 7, 50, 50)
        gray = cv2.resize(gray, None, fx=2.0, fy=2.0, interpolation=cv2.INTER_CUBIC)

        ocr_res = self.reader.readtext(
            gray,
            detail=1,
            allowlist=ALLOWED  # :contentReference[oaicite:9]{index=9}
        )

        if not ocr_res:
            return "", det_conf, 0.0, crop

        best = max(ocr_res, key=lambda x: x[2])
        text_raw = best[1]
        ocr_conf = float(best[2])

        return self._clean(text_raw), det_conf, ocr_conf, crop
