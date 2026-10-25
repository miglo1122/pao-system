"""Формування проєктів адміністративних актів із рішень ядра."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

KYIV = ZoneInfo("Europe/Kyiv")


def fmt(ts: float) -> str:
    return datetime.fromtimestamp(ts, KYIV).strftime("%d.%m.%Y %H:%M")


def render(act: dict, entity: dict) -> str:
    a = act["assessment"]
    c = a["components"]
    cls = "суттєвий" if entity.get("class") == "essential" else "важливий"
    lines = [
        f"# ПРОЄКТ № {act['number']}",
        f"## {act['kind']}",
        "",
        f"**Дата складення:** {fmt(act['t'])}  ",
        "**Наглядовий орган:** [компетентний орган з питань кіберзахисту]  ",
        f"**Адресат:** {entity['name']} ({entity.get('kind', '')}), {cls} суб'єкт",
        "",
        "### Правові підстави",
        *[f"- {b}" for b in act["legal_basis"]],
        "",
        "### Обставини, встановлені наглядачем",
        f"- Динамічний індекс кіберризику: **{a['score']}** ({a['tier_name']}), "
        f"коефіцієнт критичності C = {c['C']:.2f}.",
        f"- Вразливість E = {c['E']:.2f}; загроза T = {c['T']:.2f}; "
        f"зрілість контролів M = {c['M']:.2f}; історія інцидентів H = {c['H']:.2f}.",
        *[f"- {r[0].upper() + r[1:]}." for r in a["reasons"]],
        *[f"- Підстава термінової процедури: {r}." for r in a["urgent_reasons"]],
        "",
        "### Вимога",
        f"- {act['measure']}.",
        f"- {act['deadline_label']}: **до {fmt(act['deadline'])}**.",
        "",
        "### Права суб'єкта",
        f"- {act['rights']}.",
        "- Право ознайомитися зі складовими індексу та даними, на яких ґрунтується акт.",
        "- Оскарження в адміністративному порядку або до адміністративного суду.",
        "",
    ]
    if act["requires_human_review"]:
        lines += [
            "> **Потребує перевірки доказів посадовою особою.** Акт не може бути ухвалений "
            "лише на підставі автоматичного індексу.",
            "",
        ]
    lines.append("_Проєкт сформовано модулем PAO; підлягає підписанню уповноваженою посадовою особою._")
    return "\n".join(lines)


def write_all(acts: list[dict], registry: dict, out_dir: Path) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    ents = {e["id"]: e for e in registry["entities"]}
    paths = []
    for act in acts:
        num = int(act["number"].split("-")[-1])
        p = out_dir / f"PAO-{num:04d}_{act['entity']}.md"
        p.write_text(render(act, ents[act["entity"]]), encoding="utf-8")
        paths.append(p)
    return paths
