#!/usr/bin/env python3
"""
LushGate Web UI Mock Server
PCのブラウザで ESP32 LushGate の Web UI と REST API をシミュレーション・動作確認するためのツールです。
実行方法: python3 tools/mock_web_server.py
アクセス: http://localhost:8080
"""

import http.server
import json
import os
import time

PORT = 8080
WEB_DIR = os.path.join(os.path.dirname(__file__), "..", "main", "web")

# モック用ステート
mock_state = {
    "config": {
        "sleep_interval_sec": 180,
        "sched_hour": 7,
        "sched_min": 0,
        "rain_thresh_min": 60,
        "adc_thresh_mv": 1500,
        "pump_on_sec": 180,
        "pump_off_sec": 120,
        "pump_total_sec": 600,
        "ap_timeout_sec": 300,
    },
    "history": [
        {"timestamp": int(time.time()) - 86400 * 3, "rain_accum_min": 15, "pump_run_sec": 600, "result": 0, "battery_mv": 12800},
        {"timestamp": int(time.time()) - 86400 * 2, "rain_accum_min": 85, "pump_run_sec": 0, "result": 1, "battery_mv": 12750},
        {"timestamp": int(time.time()) - 86400 * 1, "rain_accum_min": 0, "pump_run_sec": 600, "result": 0, "battery_mv": 12700},
    ],
    "rain_accum_min": 25,
    "sensor_raw": 1240,
    "sensor_mv": 980,
    "is_raining": False,
    "pump_running": False
}

class MockHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            with open(os.path.join(WEB_DIR, "index.html"), "rb") as f:
                self.wfile.write(f.read())
            return
        
        elif self.path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            cfg = mock_state["config"]
            data = {
                "time": int(time.time()),
                "rain_accum_min": mock_state["rain_accum_min"],
                "rain_thresh_min": cfg["rain_thresh_min"],
                "sched_hour": cfg["sched_hour"],
                "sched_min": cfg["sched_min"],
                "pump_total_sec": cfg["pump_total_sec"],
                "sensor_raw": mock_state["sensor_raw"],
                "sensor_mv": mock_state["sensor_mv"],
                "is_raining": mock_state["is_raining"]
            }
            self.wfile.write(json.dumps(data).encode("utf-8"))
            return

        elif self.path == "/api/config":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(mock_state["config"]).encode("utf-8"))
            return

        elif self.path == "/api/history":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(mock_state["history"]).encode("utf-8"))
            return

        elif self.path == "/api/history/csv":
            self.send_response(200)
            self.send_header("Content-Type", "text/csv")
            self.send_header("Content-Disposition", 'attachment; filename="lushgate_history.csv"')
            self.end_headers()
            csv_lines = ["Timestamp,RainAccumMin,Result,PumpRunSec,BatteryMv\n"]
            for h in mock_state["history"]:
                csv_lines.append(f"{h['timestamp']},{h['rain_accum_min']},{h['result']},{h['pump_run_sec']},{h['battery_mv']}\n")
            self.wfile.write("".join(csv_lines).encode("utf-8"))
            return

        self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else "{}"
        try:
            req_data = json.loads(body) if body else {}
        except Exception:
            req_data = {}

        if self.path == "/api/time":
            print(f"[Mock] RTC Time synced to epoch: {req_data.get('epoch')}")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            return

        elif self.path == "/api/config":
            print(f"[Mock] Updating config: {req_data}")
            mock_state["config"].update(req_data)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            return

        elif self.path == "/api/history/clear":
            print("[Mock] History cleared")
            mock_state["history"] = []
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            return

        elif self.path == "/api/pump/test":
            action = req_data.get("action")
            duration = req_data.get("duration_sec", 30)
            print(f"[Mock] Pump action: {action}, duration: {duration}s")
            mock_state["pump_running"] = (action == "start")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            return

        elif self.path == "/api/system/sleep":
            print("[Mock] System sleep requested")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            return

        self.send_error(404)

if __name__ == "__main__":
    print(f"🌱 LushGate Mock Server running at http://localhost:{PORT}")
    print("Press Ctrl+C to stop.")
    server = http.server.HTTPServer(("0.0.0.0", PORT), MockHandler)
    server.serve_forever()
