"""Замкнений цикл нагляду: ядро ухвалює рішення, а модельні суб'єкти й наглядач реагують.

* Суб'єкт, який отримав запит (Р1) або вимогу (Р3), з певною ймовірністю
  покращує контролі, і це надходить новою телеметрією.
* Наглядач на рівні Р2+ призначає аудит на кінцевий строк. Результат аудиту
  визначає прихована «справжня» схильність до порушень, якої ядро не бачить.
  Саме ці результати потім використовує калібрування ваг індексу.
"""

from __future__ import annotations

import heapq
import itertools
import math
import random
from dataclasses import dataclass, field

from .core import Engine

HOUR, DAY = 3600, 86400


def true_deficiency_probability(E: float, M: float, H: float) -> float:
    """Прихована модель «реальності»: порушення залежать від вразливостей і слабких
    контролів, а не від інтенсивності загроз. Калібрування має це виявити."""
    z = 5.0 * (0.55 * E + 0.85 * (1 - M) + 0.25 * H - 0.62)
    return 1 / (1 + math.exp(-z))


@dataclass
class RunResult:
    decisions: list[dict] = field(default_factory=list)
    acts: list[dict] = field(default_factory=list)
    audits: list[dict] = field(default_factory=list)
    timeline: list[dict] = field(default_factory=list)
    snapshot: dict = field(default_factory=dict)
    events_in: int = 0


def run(engine: Engine, events: list[dict], seed: int = 11, on_decision=None) -> RunResult:
    rng = random.Random(seed)
    res = RunResult()
    counter = itertools.count()
    queue = [(e["t"], next(counter), e) for e in events]
    heapq.heapify(queue)
    audit_planned: set[tuple[str, int]] = set()
    last_day = None
    mirror = {e["id"]: [e["E"], e["M"]] for e in engine.snapshot()["entities"]}

    def sync(eid):
        ent = next(x for x in engine.snapshot()["entities"] if x["id"] == eid)
        mirror[eid] = [ent["E"], ent["M"]]

    while queue:
        t, _, ev = heapq.heappop(queue)
        features = None
        if ev["type"] == "telemetry":
            eid = ev["entity"]
            if eid in mirror:
                if "dE" in ev or "dM" in ev:
                    E, M = mirror[eid]
                    ev = {
                        "type": "telemetry",
                        "t": t,
                        "entity": eid,
                        "E": round(min(1, max(0, E + ev.get("dE", 0))), 4),
                        "M": round(min(1, max(0, M + ev.get("dM", 0))), 4),
                    }
                mirror[eid] = [ev.get("E", mirror[eid][0]), ev.get("M", mirror[eid][1])]
        if ev["type"] == "audit":  # знімок складових індексу в момент аудиту
            snap = engine.snapshot()
            ent = next(x for x in snap["entities"] if x["id"] == ev["entity"])
            a = ent["assessment"]
            features = a["components"] | {"score": a["score"], "tier": a["tier"]}
            p = true_deficiency_probability(ent["E"], ent["M"], a["components"]["H"])
            ev["outcome"] = "confirmed" if rng.random() < p else "cleared"
            if ev["outcome"] == "confirmed":
                ev["M"] = round(min(1, ent["M"] + 0.15), 3)  # усунення за вимогою аудиту
        res.events_in += 1
        out = engine.process(ev)
        if features is not None:
            res.audits.append({"t": t, "entity": ev["entity"], "outcome": ev["outcome"], **features})
            sync(ev["entity"])

        for d in out:
            res.decisions.append(d)
            if on_decision:
                on_decision(d)
            act = d.get("act")
            if act:
                res.acts.append({"t": d["t"], "entity": d["entity"], "decision": d["type"], **act})
            if d["type"] == "escalation":
                eid, tier = d["entity"], d["to"]
                if tier in (1, 3) and rng.random() < 0.75:  # суб'єкт виконує вимогу
                    gain = rng.uniform(0.06, 0.14) * (1.5 if tier == 3 else 1)
                    resp = {
                        "type": "telemetry",
                        "t": t + rng.uniform(24, 70) * HOUR,
                        "entity": eid,
                        "dM": round(gain, 4),
                        "dE": round(-gain / 2, 4),
                        "source": "відповідь суб'єкта",
                    }
                    heapq.heappush(queue, (resp["t"], next(counter), resp))
                if tier >= 2 and (eid, tier) not in audit_planned:
                    audit_planned.add((eid, tier))
                    due = act["deadline"] if act else t + 5 * DAY
                    au = {"type": "audit", "t": due + rng.uniform(1, 30) * HOUR, "entity": eid}
                    heapq.heappush(queue, (au["t"], next(counter), au))
            if d["type"] == "deescalation":
                audit_planned = {k for k in audit_planned if k[0] != d["entity"]}

        day = int(t // DAY)
        if day != last_day and ev["type"] == "tick":
            last_day = day
            snap = engine.snapshot()
            row = {
                "t": t,
                "tiers": [0] * 5,
                "urgent": 0,
                "scores": {e["id"]: e["assessment"]["score"] for e in snap["entities"]},
            }
            for e in snap["entities"]:
                row["tiers"][e["assessment"]["tier"]] += 1
                row["urgent"] += 1 if e["urgent"] else 0
            res.timeline.append(row)

    res.snapshot = engine.snapshot()
    return res
