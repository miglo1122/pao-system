"""Калібрування ваг індексу за результатами аудитів (вимога «responsive regulator»).

Логістична регресія з L2-регуляризацією (numpy, градієнтний спуск) оцінює,
наскільки кожна складова індексу передбачає підтверджене порушення. Додатні
коефіцієнти, нормовані до суми 1, дають запропоновані ваги. Окремо обчислюється
AUC поточного індексу та каліброваної моделі: чи краще нове правило розрізняє
суб'єктів з порушеннями та без них.
"""

from __future__ import annotations

import numpy as np

FEATURES = ("E", "T", "1-M", "H")


def _matrix(audits: list[dict]) -> tuple[np.ndarray, np.ndarray]:
    X = np.array([[a["E"], a["T"], 1 - a["M"], a["H"]] for a in audits], dtype=float)
    y = np.array([1.0 if a["outcome"] == "confirmed" else 0.0 for a in audits])
    return X, y


def auc(scores: np.ndarray, y: np.ndarray) -> float:
    pos, neg = scores[y == 1], scores[y == 0]
    if len(pos) == 0 or len(neg) == 0:
        return float("nan")
    gt = (pos[:, None] > neg[None, :]).sum() + 0.5 * (pos[:, None] == neg[None, :]).sum()
    return float(gt / (len(pos) * len(neg)))


def fit_logistic(X: np.ndarray, y: np.ndarray, l2: float = 0.05, iters: int = 6000, lr: float = 0.5):
    Xb = np.hstack([np.ones((len(X), 1)), X])
    w = np.zeros(Xb.shape[1])
    for _ in range(iters):
        p = 1 / (1 + np.exp(-Xb @ w))
        grad = Xb.T @ (p - y) / len(y)
        grad[1:] += l2 * w[1:]
        w -= lr * grad
    return w


def calibrate(audits: list[dict], current_weights: dict, bootstrap: int = 300, seed: int = 3) -> dict:
    if len(audits) < 10:
        return {
            "status": "insufficient",
            "n": len(audits),
            "message": "Замало аудитів для калібрування (потрібно щонайменше 10).",
        }
    X, y = _matrix(audits)
    w = fit_logistic(X, y)
    coef = w[1:]
    pos = np.clip(coef, 0, None)
    suggested = pos / pos.sum() if pos.sum() > 0 else np.full(4, 0.25)

    rng = np.random.default_rng(seed)
    boots = []
    for _ in range(bootstrap):
        idx = rng.integers(0, len(y), len(y))
        if y[idx].min() == y[idx].max():
            continue
        c = np.clip(fit_logistic(X[idx], y[idx], iters=1500)[1:], 0, None)
        boots.append(c / c.sum() if c.sum() > 0 else np.full(4, 0.25))
    boots = np.array(boots) if boots else np.array([suggested])
    lo, hi = np.percentile(boots, 5, axis=0), np.percentile(boots, 95, axis=0)

    cw = np.array([current_weights[k] for k in ("E", "T", "M", "H")])
    cur_score = X @ cw
    new_score = X @ suggested
    return {
        "status": "ok",
        "n": int(len(y)),
        "confirmed": int(y.sum()),
        "precision": float(y.mean()),
        "features": list(FEATURES),
        "coefficients": [float(c) for c in coef],
        "current_weights": [float(x) for x in cw],
        "suggested_weights": [float(x) for x in suggested],
        "ci90_low": [float(x) for x in lo],
        "ci90_high": [float(x) for x in hi],
        "auc_current": auc(cur_score, y),
        "auc_suggested": auc(new_score, y),
        "message": _message(suggested, cw, y.mean()),
    }


def _message(sug: np.ndarray, cur: np.ndarray, precision: float) -> str:
    names = {"E": "вразливість", "T": "загроза", "1-M": "незрілість контролів", "H": "історія інцидентів"}
    diff = sug - cur
    up, down = FEATURES[int(diff.argmax())], FEATURES[int(diff.argmin())]
    s = (
        f"Частка підтверджених ескалацій {precision:.0%}. Дані аудитів радять збільшити вагу "
        f"«{names[up]}» і зменшити вагу «{names[down]}»."
    )
    if down == "T":
        s += (
            " Загроза визначає пріоритет уваги, але слабко передбачає порушення конкретним "
            "суб'єктом; її доцільно використовувати для термінової процедури, а не для санкцій."
        )
    return s
