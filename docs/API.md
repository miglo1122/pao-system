# Інтерфейси системи PAO

Система має три рівні доступу: REST API для інтеграцій і панелі, Python API для аналітики та CLI ядра для конвеєрів обробки даних.

## 1. REST API

Сервер запускається командою `make serve` або `python -m pao serve [--host 0.0.0.0] [--port 8080] [--db pao.db] [--demo 12]`. Усі відповіді мають формат JSON у кодуванні UTF-8.

### `GET /api/health`
```json
{"status": "ok", "backend": "pybind11"}
```

### `GET /api/snapshot`
Повертає поточний стан: `now`, `config`, `hubs`, `threats`, `entities[]` (кожен з `assessment`) та `incidents[]` (зі станом етапів звітування).

### `GET /api/decisions?limit=100`
Повертає останні рішення ядра, від найновіших.

### `GET /api/acts`
Повертає проєкти актів: `number`, `t`, `entity_id`, `kind`, `deadline`, `requires_human_review`, `status`, `reviewer`.

### `GET /api/transparency`
```json
{"escalations": 33, "urgent_procedures": 13, "reporting_breaches": 4,
 "audits": 18, "audits_cleared": 4, "acts_human_review": 17}
```

### `GET /api/cascade?runs=20000`
Повертає результат моделювання каскадів (`seeds[]`, `mitigation[]`, `systemic_risk`, `simulations`, `seconds`). Параметр `runs` обмежено значенням 100 000.

### `POST /api/events`
Тіло запиту — одна подія або масив подій (формат описано в [ARCHITECTURE.md](ARCHITECTURE.md#4-модель-даних)). Якщо `t` не вказано, використовується поточний час системи.

```bash
curl -X POST localhost:8080/api/events -H 'Content-Type: application/json' -d '{
  "type": "incident", "id": "INC-42", "entity": "p2",
  "supplier": "ТОС «ТерміналСофт»", "hub_wide": true, "early_warning": true}'
```

Відповідь — масив рішень ядра. Невідомий тип події дає `400`.

### `POST /api/acts/{номер}/status`
```json
{"status": "approved", "reviewer": "інспектор О. Коваль"}
```
Допустимі статуси: `approved`, `rejected`, `appealed`, `executed`. Якщо акт потребує перевірки людиною, затвердити його без `reviewer` не можна: сервер відповідає `422`.

## 2. Python API

```python
from pao import Engine, load_registry

engine = Engine(load_registry())                       # реєстр із data/registry.json
decisions = engine.process({"type": "tick", "t": 1792994400})
decisions += engine.process_many([
    {"type": "incident", "t": 1792998000, "id": "I-1", "entity": "r1"},
    {"type": "tick", "t": 1793090000},                 # через 25 год без звіту
])
for d in decisions:
    print(d["type"], d.get("entity"), d.get("to"))

state = engine.snapshot()                              # оцінки всіх суб'єктів
ranking = engine.cascade(runs=20000)                   # системні точки відмови
```

Інші модулі:

| Модуль | Функція |
| --- | --- |
| `pao.simulate.exogenous_events(registry, days, seed)` | Події сценарію «Осінь-2026» |
| `pao.pipeline.run(engine, events, seed)` | Замкнений цикл нагляду → `RunResult` |
| `pao.calibrate.calibrate(audits, weights)` | Запропоновані ваги, інтервали, AUC |
| `pao.acts.render(act, entity)` | Текст проєкту акта (Markdown) |
| `pao.report.build(result, cascade, calib, transparency, perf)` | HTML-звіт |
| `pao.reference.verify_snapshot(snapshot)` | Звірка з еталонною формулою |

## 3. CLI ядра

```
pao_engine stream  --registry R.json [--config C.json] [--snapshot S.json]  < events.jsonl > decisions.jsonl
pao_engine assess  --registry R.json [--config C.json]
pao_engine cascade --registry R.json [--runs N] [--seed S] [--threads K] [--no-mitigation]
pao_engine bench   --registry R.json [--events N]
```

Ядро читає й пише формат JSON Lines: один об'єкт у рядку. Рядки, що починаються з `#`, ігноруються. Код завершення 2 означає помилку параметрів або файлу.

### Конфігурація (`--config`)

```json
{
  "weights": {"E": 0.30, "T": 0.25, "M": 0.30, "H": 0.15},
  "thresholds": [30, 50, 70, 85],
  "c_essential": 1.0, "c_important": 0.6, "coordinator_mult": 1.1,
  "supplier_compromise_threat": 0.85, "hub_peer_threat": 0.70
}
```

### Реєстр (`--registry`)

```json
{
  "t0": 1792994400,
  "threats": {"sea": 0.55, "rail": 0.60},
  "hubs": [{"id": "black", "name": "Чорноморський хаб", "coordinator": "p1"}],
  "entities": [{"id": "p1", "name": "…", "mode": "sea", "class": "essential", "hub": "black",
                "suppliers": ["PCS «ПортЛінк»"], "E": 0.35, "M": 0.62, "H0": 0.10}]
}
```
