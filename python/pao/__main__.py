"""Командний рядок системи PAO.

python -m pao demo     повний цикл: симуляція 30 днів → рішення → акти → каскади → калібрування → звіт
python -m pao serve    REST API та веб-панель наглядача (http://127.0.0.1:8080)
python -m pao cascade  моделювання каскадних атак і пріоритет аудитів
python -m pao verify   диференційна перевірка C++ ↔ Python
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from . import acts, calibrate, pipeline, reference, report, simulate
from .core import ROOT, Engine, backend, find_cli, load_registry
from .store import Store

DEFAULT_REGISTRY = ROOT / "data" / "registry.json"


def step(n: int, total: int, text: str) -> None:
    print(f"[{n}/{total}] {text}")


def native_benchmark() -> float | None:
    """Пропускна здатність C++ ядра без накладних витрат Python (подій за секунду)."""
    cli = find_cli()
    if not cli:
        return None
    cmd = [cli, "bench", "--registry", str(DEFAULT_REGISTRY)]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return json.loads(out)["events_per_second"]


def collect_audits(registry: dict, days: int, replications: int) -> list[dict]:
    """Повторює сценарій з іншими випадковостями, щоб отримати вибірку аудитів для калібрування."""
    audits: list[dict] = []
    for r in range(replications):
        seed = 1000 + r
        events = simulate.exogenous_events(registry, days=days, seed=seed)
        audits += pipeline.run(Engine(registry), events, seed=seed).audits
    return audits


def cmd_demo(args: argparse.Namespace) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    registry = load_registry(args.registry)
    step(1, 6, f"Ядро: {backend()}")

    engine = Engine(registry)
    events = simulate.exogenous_events(registry, days=args.days, seed=args.seed)
    started = time.perf_counter()
    result = pipeline.run(engine, events, seed=args.seed)
    elapsed = time.perf_counter() - started
    step(
        2,
        6,
        f"Оброблено {result.events_in} подій за {elapsed:.2f} с: {len(result.decisions)} рішень, "
        f"{len(result.acts)} проєктів актів, {len(result.audits)} аудитів",
    )

    errors = reference.verify_snapshot(result.snapshot)
    step(3, 6, f"Диференційна перевірка C++ ↔ Python: {errors or 'збігається'}")

    db_path = out / "pao.db"
    db_path.unlink(missing_ok=True)
    store = Store(db_path)
    store.load_registry(registry)
    store.add_events(events)
    store.add_decisions(result.decisions)
    store.add_audits(result.audits)
    acts.write_all(result.acts, registry, out / "acts")

    cascade = engine.cascade(runs=args.runs)
    step(
        4,
        6,
        f"Каскади: {cascade['simulations']:,} симуляцій за {cascade['seconds']:.2f} с. "
        f"Найбільша точка відмови: {cascade['seeds'][0]['label']}",
    )

    audits = result.audits + collect_audits(registry, args.days, args.replications)
    calibration = calibrate.calibrate(audits, engine.snapshot()["config"]["weights"])
    step(5, 6, f"Калібрування на {len(audits)} аудитах: {calibration.get('message', '')}")

    perf = {"backend": backend(), "events_per_second": native_benchmark() or result.events_in / elapsed}
    transparency = store.transparency()
    (out / "report.html").write_text(report.build(result, cascade, calibration, transparency, perf), encoding="utf-8")
    summary = {
        "events": result.events_in,
        "decisions": len(result.decisions),
        "acts": len(result.acts),
        "audits_main_run": len(result.audits),
        "transparency": transparency,
        "calibration": calibration,
        "cascade_top": cascade["seeds"][:5],
        "mitigation_top": cascade["mitigation"][:5],
        "differential_errors": errors,
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    step(6, 6, f"Готово: {out / 'report.html'}, {db_path}, {out / 'acts'} ({len(result.acts)} файлів)")
    return 1 if errors else 0


def cmd_serve(args: argparse.Namespace) -> int:
    from .server import App, serve

    registry = load_registry(args.registry)
    engine = Engine(registry)
    store = Store(args.db)
    store.load_registry(registry)
    app = App(engine, store, registry)
    if args.demo:
        horizon = registry["t0"] + args.demo * 86400
        events = [e for e in simulate.exogenous_events(registry, days=30, seed=7) if e["t"] <= horizon]
        result = pipeline.run(engine, events, seed=7)
        store.add_events(events)
        store.add_decisions(result.decisions)
        store.add_audits(result.audits)
        app.recent = result.decisions[::-1][:500]
        print(f"Відтворено {args.demo} днів сценарію: {len(result.decisions)} рішень")
    serve(app, args.host, args.port)
    return 0


def cmd_cascade(args: argparse.Namespace) -> int:
    ranking = Engine(load_registry(args.registry)).cascade(runs=args.runs)
    for s in ranking["seeds"][:10]:
        print(
            f"{s['expected_impact']:6.2f}  P(хаб)={s['p_hub_disruption']:.0%}  хабів={s['hubs_reached']}  {s['label']}"
        )
    print("Пріоритет аудитів:")
    for m in ranking["mitigation"][:5]:
        print(f"  −{m['reduction_pct']:4.1f}%  {m['name']}")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    """Після кожної події порівнює індекс ядра з незалежною Python-реалізацією формули."""
    registry = load_registry(args.registry)
    engine = Engine(registry)
    events = simulate.exogenous_events(registry, days=30, seed=args.seed)
    mismatches = 0
    for event in events:
        engine.process(event)
        for err in reference.verify_snapshot(engine.snapshot()):
            mismatches += 1
            print("  ", err)
    checked = len(events) * len(registry["entities"])
    if mismatches:
        print(f"Розбіжностей: {mismatches} з {checked} перевірок")
    else:
        print(f"C++ і Python дають однаковий індекс: {checked} перевірок ({len(events)} подій)")
    return 1 if mismatches else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="pao", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--registry", default=str(DEFAULT_REGISTRY), help="JSON-реєстр суб'єктів і хабів")
    sub = parser.add_subparsers(dest="cmd", required=True)

    demo = sub.add_parser("demo", help="повний демонстраційний цикл і звіт")
    demo.add_argument("--out", default=str(ROOT / "out"))
    demo.add_argument("--days", type=int, default=30)
    demo.add_argument("--seed", type=int, default=7)
    demo.add_argument("--runs", type=int, default=20000, help="симуляцій Монте-Карло на вузол")
    demo.add_argument("--replications", type=int, default=40, help="повторень сценарію для калібрування")

    srv = sub.add_parser("serve", help="REST API та веб-панель")
    srv.add_argument("--host", default="127.0.0.1")
    srv.add_argument("--port", type=int, default=8080)
    srv.add_argument("--db", default=":memory:")
    srv.add_argument("--demo", type=int, default=12, help="днів сценарію перед стартом (0 = порожній стан)")

    sub.add_parser("cascade", help="рейтинг системних точок відмови").add_argument("--runs", type=int, default=20000)
    sub.add_parser("verify", help="звірка C++ і Python").add_argument("--seed", type=int, default=7)
    return parser


COMMANDS = {"demo": cmd_demo, "serve": cmd_serve, "cascade": cmd_cascade, "verify": cmd_verify}


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return COMMANDS[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
