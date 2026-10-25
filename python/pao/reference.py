"""Незалежна еталонна реалізація формули DCRI на Python.

Використовується для диференційного тестування: обидві реалізації (C++ і Python)
мають давати однаковий індекс для кожного суб'єкта на кожному кроці.
"""

from __future__ import annotations


def dcri(components: dict, weights: dict) -> int:
    E, T, M, H, C = (components[k] for k in ("E", "T", "M", "H", "C"))
    raw = 100 * C * (weights["E"] * E + weights["T"] * T + weights["M"] * (1 - M) + weights["H"] * H)
    return int(min(100.0, raw) + 0.5)  # округлення половини вгору, як std::lround для додатних


def tier(score: int, thresholds=(30, 50, 70, 85)) -> int:
    return sum(score >= t for t in thresholds)


def verify_snapshot(snapshot: dict) -> list[str]:
    w = snapshot["config"]["weights"]
    th = snapshot["config"]["thresholds"]
    errors = []
    for e in snapshot["entities"]:
        a = e["assessment"]
        s = dcri(a["components"], w)
        if s != a["score"]:
            errors.append(f"{e['id']}: C++ {a['score']} ≠ Python {s}")
        raw_tier = tier(s, th)
        if a["tier"] > max(raw_tier, 1 if a["overdue_stages"] else 0):
            errors.append(f"{e['id']}: рівень {a['tier']} вищий за дозволений індексом")
    return errors
