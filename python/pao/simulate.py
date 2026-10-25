"""Генератор зовнішніх подій для 30-денного сценарію «Осінь-2026».

Сценарій відтворює типові для України загрози транспорту: DDoS на продаж квитків,
шифрувальник у системі управління терміналом, компрометацію хмарного провайдера,
який обслуговує суб'єктів у трьох хабах, і витік даних складської системи.
Частина суб'єктів подає звіти із запізненням, щоб перевірити годинник NIS2.
"""

from __future__ import annotations

import random

HOUR, DAY = 3600, 86400

INCIDENTS = [
    # день, година, суб'єкт, постачальник, вплив на хаб, назва, (затримки звітів у годинах; None = не подано)
    (
        2,
        9,
        "r1",
        "Квиткова платформа «РейлСервіс»",
        False,
        "Масована DDoS-атака на онлайн-продаж квитків",
        (3, 60, 26 * 24),
    ),
    (
        8,
        4,
        "p2",
        "ТОС «ТерміналСофт»",
        True,
        "Шифрувальник у системі управління терміналом, зупинка двох причалів",
        (30, 80, 25 * 24),
    ),
    (15, 22, "t1", "Хмара «УкрДата»", True, "Компрометація облікових записів хмарного провайдера", (20, 70, 13 * 24)),
    (22, 14, "w1", "WMS «СкладПро»", False, "Витік даних про вантажі зі складської системи", (None, None, None)),
]

THREATS = [  # день, вид транспорту, рівень
    (1, "rail", 0.80),
    (6, "rail", 0.60),
    (7, "sea", 0.75),
    (14, "road", 0.55),
    (19, "sea", 0.55),
    (24, "road", 0.40),
]


def exogenous_events(registry: dict, days: int = 30, seed: int = 7) -> list[dict]:
    rng = random.Random(seed)
    t0 = registry["t0"]
    ev: list[dict] = []
    ids = [e["id"] for e in registry["entities"]]

    # Телеметрія надходить як прирости (dE, dM); конвеєр перетворює їх на абсолютні
    # значення з урахуванням того, що суб'єкт уже виправив за вимогами наглядача.
    for d in range(days + 1):
        ev.append({"type": "tick", "t": t0 + d * DAY})
        for h in (0, 6, 12, 18):  # кожні 6 годин
            t = t0 + d * DAY + h * HOUR + rng.randint(0, 1800)
            for eid in ids:
                ev.append(
                    {
                        "type": "telemetry",
                        "t": t,
                        "entity": eid,
                        "dE": round(rng.gauss(0.004, 0.02), 4),  # нові вразливості накопичуються
                        "dM": round(rng.gauss(0.0, 0.008), 4),
                    }
                )

    for d, mode, lvl in THREATS:
        ev.append(
            {
                "type": "threat",
                "t": t0 + d * DAY + 8 * HOUR,
                "mode": mode,
                "level": lvl,
                "source": "CERT-UA / партнери ЄС",
            }
        )

    for n, (d, h, ent, sup, hub_wide, title, delays) in enumerate(INCIDENTS, start=1):
        start = t0 + d * DAY + h * HOUR
        iid = f"INC-2026-{100 + n}"
        ev.append(
            {
                "type": "incident",
                "t": start,
                "id": iid,
                "entity": ent,
                "supplier": sup,
                "significant": True,
                "hub_wide": hub_wide,
                "title": title,
            }
        )
        for stage, delay in zip(("early", "notification", "final"), delays):
            if delay is not None:
                ev.append({"type": "report", "t": start + delay * HOUR, "incident": iid, "stage": stage})
        if delays[2] is not None:
            ev.append({"type": "close", "t": start + delays[2] * HOUR + 2 * HOUR, "incident": iid})

    ev.sort(key=lambda e: (e["t"], e["type"] != "tick"))
    return ev
