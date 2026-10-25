"""REST API та веб-панель наглядача (лише стандартна бібліотека Python).

GET  /api/health            стан і тип ядра
GET  /api/snapshot          реєстр, оцінки, інциденти
GET  /api/decisions?limit=N останні рішення
GET  /api/acts              проєкти актів зі статусами
GET  /api/transparency      звіт про прозорість
GET  /api/cascade?runs=N    моделювання каскадних атак
POST /api/events            подія або масив подій → рішення ядра
POST /api/acts/<№>/status   {"status": "approved", "reviewer": "..."}
"""

from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from .core import ROOT, Engine

WEB = ROOT / "web"
ALLOWED_EVENTS = {"tick", "telemetry", "threat", "incident", "report", "close", "audit", "register"}


class App:
    def __init__(self, engine: Engine, store, registry: dict):
        self.engine, self.store, self.registry = engine, store, registry
        self.lock = threading.Lock()
        self.recent: list[dict] = []

    def submit(self, events: list[dict]) -> list[dict]:
        for ev in events:
            if not isinstance(ev, dict) or ev.get("type") not in ALLOWED_EVENTS:
                raise ValueError(f"недопустима подія: {ev!r:.80}")
        with self.lock:
            now = self.engine.snapshot()["now"]
            for ev in events:
                ev.setdefault("t", now)
            out = self.engine.process_many(events)
            self.store.add_events(events)
            self.store.add_decisions(out)
            self.recent = (out[::-1] + self.recent)[:500]
            return out


def make_handler(app: App):
    class H(BaseHTTPRequestHandler):
        server_version = "PAO/1.0"

        def log_message(self, *a):
            pass

        def _send(self, code: int, body, ctype="application/json; charset=utf-8"):
            data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            try:
                if u.path in ("/", "/index.html"):
                    return self._send(200, (WEB / "index.html").read_bytes(), "text/html; charset=utf-8")
                if u.path == "/api/health":
                    return self._send(200, {"status": "ok", "backend": app.engine.mode})
                if u.path == "/api/snapshot":
                    with app.lock:
                        return self._send(200, app.engine.snapshot())
                if u.path == "/api/decisions":
                    n = int(q.get("limit", ["100"])[0])
                    return self._send(200, app.recent[:n])
                if u.path == "/api/acts":
                    rows = app.store.query(
                        "SELECT number,t,entity_id,kind,deadline,requires_human_review,status,reviewer "
                        "FROM acts ORDER BY t DESC LIMIT 200"
                    )
                    return self._send(200, rows)
                if u.path == "/api/transparency":
                    return self._send(200, app.store.transparency())
                if u.path == "/api/cascade":
                    runs = min(100000, int(q.get("runs", ["10000"])[0]))
                    with app.lock:
                        return self._send(200, app.engine.cascade(runs=runs))
                return self._send(404, {"error": "не знайдено"})
            except Exception as ex:  # noqa: BLE001
                return self._send(500, {"error": str(ex)})

        def do_POST(self):
            u = urlparse(self.path)
            try:
                n = int(self.headers.get("Content-Length", "0"))
                if n > 2_000_000:
                    return self._send(413, {"error": "запит завеликий"})
                body = json.loads(self.rfile.read(n) or b"null")
                if u.path == "/api/events":
                    events = body if isinstance(body, list) else [body]
                    return self._send(200, app.submit(events))
                parts = u.path.strip("/").split("/")
                if len(parts) == 4 and parts[:2] == ["api", "acts"] and parts[3] == "status":
                    status, reviewer = body.get("status"), (body.get("reviewer") or "").strip()
                    act = app.store.query("SELECT requires_human_review FROM acts WHERE number=?", (parts[2],))
                    if not act:
                        return self._send(404, {"error": "акт не знайдено"})
                    if status == "approved" and act[0]["requires_human_review"] and not reviewer:
                        return self._send(422, {"error": "Цей акт потребує перевірки доказів: вкажіть посадову особу."})
                    ok = app.store.set_act_status(parts[2], status, reviewer or None)
                    return self._send(200 if ok else 400, {"ok": ok})
                return self._send(404, {"error": "не знайдено"})
            except (ValueError, json.JSONDecodeError) as ex:
                return self._send(400, {"error": str(ex)})
            except Exception as ex:  # noqa: BLE001
                return self._send(500, {"error": str(ex)})

    return H


def serve(app: App, host="127.0.0.1", port=8080):
    httpd = ThreadingHTTPServer((host, port), make_handler(app))
    print(f"Панель PAO: http://{host}:{port}  (ядро: {app.engine.mode})")
    httpd.serve_forever()
