"""Доступ до C++ ядра.

Спершу пробує вбудований модуль ``_paocore`` (pybind11, без накладних витрат),
інакше запускає консольний ``pao_engine`` як підпроцес. Обидва шляхи мають
однаковий інтерфейс, тому решта системи не залежить від способу збирання.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from collections.abc import Iterable
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

try:  # pragma: no cover - залежить від збирання
    from . import _paocore  # type: ignore
except ImportError:  # pragma: no cover
    _paocore = None


def find_cli() -> str | None:
    for cand in (os.environ.get("PAO_ENGINE"), ROOT / "build" / "pao_engine", shutil.which("pao_engine")):
        if cand and Path(cand).is_file():
            return str(cand)
    return None


def backend() -> str:
    if _paocore is not None:
        return "pybind11"
    return "cli" if find_cli() else "missing"


class Engine:
    """Обгортка над ядром PAO. Події й рішення — звичайні словники Python."""

    def __init__(self, registry: dict, config: dict | None = None, force_cli: bool = False):
        self.registry = registry
        self.config = config or {}
        self._native = None
        self._pending: list[dict] = []
        self._cli_log: list[dict] = []
        if _paocore is not None and not force_cli:
            self._native = _paocore.Engine(
                json.dumps(registry, ensure_ascii=False), json.dumps(self.config, ensure_ascii=False)
            )
        elif not find_cli():
            raise RuntimeError("Ядро PAO не зібрано. Виконайте: make build")

    @property
    def mode(self) -> str:
        return "pybind11" if self._native else "cli"

    def process(self, event: dict) -> list[dict]:
        return self.process_many([event])

    def process_many(self, events: Iterable[dict]) -> list[dict]:
        events = list(events)
        if self._native:
            out = self._native.process_many([json.dumps(e, ensure_ascii=False) for e in events])
            return [json.loads(x) for x in out]
        # CLI без стану між викликами: відтворюємо весь журнал подій
        self._cli_log.extend(events)
        decisions, snap = self._run_cli(self._cli_log)
        self._snapshot = snap
        seen = getattr(self, "_seen", 0)
        self._seen = len(decisions)
        return decisions[seen:]

    def snapshot(self) -> dict:
        if self._native:
            return json.loads(self._native.snapshot())
        if not hasattr(self, "_snapshot"):
            self._run_cli_and_store()
        return self._snapshot

    def cascade(self, runs: int = 20000, seed: int = 20260324, mitigation: bool = True, **params) -> dict:
        if self._native:
            p = {"runs": runs, "seed": seed, **params}
            return json.loads(self._native.cascade(json.dumps(p), mitigation))
        with tempfile.TemporaryDirectory() as d:
            reg = self._current_registry(Path(d))
            cmd = [find_cli(), "cascade", "--registry", str(reg), "--runs", str(runs), "--seed", str(seed)]
            if not mitigation:
                cmd.append("--no-mitigation")
            return json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)

    # --- CLI helpers -------------------------------------------------------
    def _current_registry(self, d: Path) -> Path:
        snap = self.snapshot()
        reg = dict(self.registry, entities=snap["entities"], threats=snap["threats"], t0=snap["now"])
        p = d / "registry.json"
        p.write_text(json.dumps(reg, ensure_ascii=False), encoding="utf-8")
        return p

    def _run_cli_and_store(self):
        decisions, snap = self._run_cli(self._cli_log)
        self._seen, self._snapshot = len(decisions), snap

    def _run_cli(self, events: list[dict]) -> tuple[list[dict], dict]:
        with tempfile.TemporaryDirectory() as d:
            reg, cfg, snap = Path(d, "r.json"), Path(d, "c.json"), Path(d, "s.json")
            reg.write_text(json.dumps(self.registry, ensure_ascii=False), encoding="utf-8")
            cfg.write_text(json.dumps(self.config, ensure_ascii=False), encoding="utf-8")
            stdin = "\n".join(json.dumps(e, ensure_ascii=False) for e in events)
            res = subprocess.run(
                [find_cli(), "stream", "--registry", str(reg), "--config", str(cfg), "--snapshot", str(snap)],
                input=stdin,
                capture_output=True,
                text=True,
                check=True,
            )
            decisions = [json.loads(line) for line in res.stdout.splitlines() if line.strip()]
            return decisions, json.loads(snap.read_text(encoding="utf-8"))


def load_registry(path: str | Path | None = None) -> dict:
    path = Path(path) if path else ROOT / "data" / "registry.json"
    return json.loads(path.read_text(encoding="utf-8"))
