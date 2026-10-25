"""Незмінний журнал подій, рішень, актів і аудитів у SQLite."""

from __future__ import annotations

import json
import sqlite3
import threading
from pathlib import Path

from .core import ROOT

SCHEMA = ROOT / "sql" / "schema.sql"


class Store:
    def __init__(self, path: str | Path = ":memory:"):
        self.db = sqlite3.connect(str(path), check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        self.db.executescript(SCHEMA.read_text(encoding="utf-8"))

    def load_registry(self, reg: dict):
        with self.lock, self.db:
            for h in reg["hubs"]:
                self.db.execute(
                    "INSERT OR REPLACE INTO hubs VALUES (?,?,?)", (h["id"], h["name"], h.get("coordinator"))
                )
            for e in reg["entities"]:
                self.db.execute(
                    "INSERT OR REPLACE INTO entities VALUES (?,?,?,?,?,?,?)",
                    (
                        e["id"],
                        e["name"],
                        e.get("kind"),
                        e["mode"],
                        e.get("class", "essential"),
                        e["hub"],
                        json.dumps(e.get("suppliers", []), ensure_ascii=False),
                    ),
                )

    def add_events(self, events):
        with self.lock, self.db:
            self.db.executemany(
                "INSERT INTO events(t,type,entity_id,payload) VALUES (?,?,?,?)",
                [(e["t"], e["type"], e.get("entity"), json.dumps(e, ensure_ascii=False)) for e in events],
            )

    def add_decisions(self, decisions):
        with self.lock, self.db:
            for d in decisions:
                self.db.execute(
                    "INSERT INTO decisions(t,type,entity_id,tier_from,tier_to,score,payload) VALUES (?,?,?,?,?,?,?)",
                    (
                        d["t"],
                        d["type"],
                        d.get("entity"),
                        d.get("from"),
                        d.get("to"),
                        d.get("score"),
                        json.dumps(d, ensure_ascii=False),
                    ),
                )
                act = d.get("act")
                if act:
                    self.db.execute(
                        "INSERT OR REPLACE INTO acts(number,t,entity_id,kind,deadline,requires_human_review,payload) "
                        "VALUES (?,?,?,?,?,?,?)",
                        (
                            act["number"],
                            d["t"],
                            d["entity"],
                            act["kind"],
                            act["deadline"],
                            int(act["requires_human_review"]),
                            json.dumps(act, ensure_ascii=False),
                        ),
                    )

    def add_audits(self, audits):
        with self.lock, self.db:
            self.db.executemany(
                "INSERT INTO audits(t,entity_id,outcome,score,e,threat,m,h) VALUES (?,?,?,?,?,?,?,?)",
                [(a["t"], a["entity"], a["outcome"], a["score"], a["E"], a["T"], a["M"], a["H"]) for a in audits],
            )

    def set_act_status(self, number: str, status: str, reviewer: str | None = None) -> bool:
        with self.lock, self.db:
            cur = self.db.execute("UPDATE acts SET status=?, reviewer=? WHERE number=?", (status, reviewer, number))
            return cur.rowcount == 1

    def query(self, sql: str, args=()) -> list[dict]:
        with self.lock:
            return [dict(r) for r in self.db.execute(sql, args).fetchall()]

    def transparency(self) -> dict:
        return self.query("SELECT * FROM transparency_report")[0]
