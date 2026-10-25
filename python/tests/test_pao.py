"""Інтеграційні тести Python-шару (unittest зі стандартної бібліотеки)."""

import json
import sys
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pao import calibrate, pipeline, reference, simulate  # noqa: E402
from pao.core import Engine, backend, find_cli, load_registry  # noqa: E402
from pao.store import Store  # noqa: E402

REG = load_registry()


class CoreTests(unittest.TestCase):
    def test_backend_available(self):
        self.assertIn(backend(), ("pybind11", "cli"))

    @unittest.skipUnless(find_cli() and backend() == "pybind11", "потрібні обидва способи доступу до ядра")
    def test_cli_and_pybind_agree(self):
        events = [e for e in simulate.exogenous_events(REG, days=10) if "dE" not in e]
        a, b = Engine(REG), Engine(REG, force_cli=True)
        da = a.process_many(events)
        db = b.process_many(events)
        self.assertEqual([d["type"] for d in da], [d["type"] for d in db])
        self.assertEqual(
            json.dumps(a.snapshot()["entities"], sort_keys=True), json.dumps(b.snapshot()["entities"], sort_keys=True)
        )

    def test_differential_reference(self):
        eng = Engine(REG)
        res = pipeline.run(eng, simulate.exogenous_events(REG, days=30), seed=7)
        self.assertEqual(reference.verify_snapshot(res.snapshot), [])
        types = {d["type"] for d in res.decisions}
        self.assertTrue({"escalation", "urgent_open", "deadline_breach"} <= types)

    def test_reporting_breach_for_unreported_incident(self):
        eng = Engine(REG)
        res = pipeline.run(eng, simulate.exogenous_events(REG, days=30), seed=7)
        breaches = [d for d in res.decisions if d["type"] == "deadline_breach"]
        self.assertTrue(any(d["incident"] == "INC-2026-104" for d in breaches))
        self.assertTrue(all(a["requires_human_review"] for a in res.acts if a["decision"] == "urgent_open"))


class CalibrationTests(unittest.TestCase):
    def test_recovers_signal_without_threat(self):
        import random

        rng = random.Random(1)
        audits = []
        for _ in range(600):
            E, T, M, H = rng.random(), rng.random(), rng.random(), rng.random() * 0.5
            p = pipeline.true_deficiency_probability(E, M, H)
            audits.append({"E": E, "T": T, "M": M, "H": H, "outcome": "confirmed" if rng.random() < p else "cleared"})
        r = calibrate.calibrate(audits, {"E": 0.3, "T": 0.25, "M": 0.3, "H": 0.15}, bootstrap=30)
        w = dict(zip(r["features"], r["suggested_weights"]))
        self.assertLess(w["T"], 0.08)
        self.assertGreater(w["1-M"], w["E"])
        self.assertGreater(r["auc_suggested"], r["auc_current"])

    def test_insufficient(self):
        self.assertEqual(calibrate.calibrate([], {"E": 1, "T": 1, "M": 1, "H": 1})["status"], "insufficient")


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from http.server import ThreadingHTTPServer

        from pao.server import App, make_handler

        st = Store()
        st.load_registry(REG)
        cls.app = App(Engine(REG), st, REG)
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(cls.app))
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()

    def call(self, path, body=None):
        req = urllib.request.Request(
            self.base + path,
            method="POST" if body is not None else "GET",
            data=None if body is None else json.dumps(body).encode(),
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_flow(self):
        code, h = self.call("/api/health")
        self.assertEqual(code, 200)
        t0 = REG["t0"]
        code, out = self.call(
            "/api/events",
            [
                {"type": "tick", "t": t0},
                {
                    "type": "incident",
                    "t": t0 + 60,
                    "id": "API-1",
                    "entity": "p1",
                    "supplier": "PCS «ПортЛінк»",
                    "hub_wide": True,
                    "early_warning": True,
                },
            ],
        )
        self.assertEqual(code, 200)
        urgent = [d for d in out if d["type"] == "urgent_open"]
        self.assertGreaterEqual(len(urgent), 3)  # координатор хабу + клієнти постачальника в інших хабах
        num = urgent[0]["act"]["number"]
        code, err = self.call(f"/api/acts/{num}/status", {"status": "approved"})
        self.assertEqual(code, 422)
        code, ok = self.call(f"/api/acts/{num}/status", {"status": "approved", "reviewer": "інспектор О. Коваль"})
        self.assertEqual(code, 200)
        code, bad = self.call("/api/events", {"type": "drop_tables"})
        self.assertEqual(code, 400)
        code, tr = self.call("/api/transparency")
        self.assertEqual(tr["urgent_procedures"], len(urgent))


if __name__ == "__main__":
    unittest.main(verbosity=2)
