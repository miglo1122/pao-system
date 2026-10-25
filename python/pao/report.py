"""Аналітичний звіт наглядача.

Будує самодостатній HTML-документ із SVG-графіками без зовнішніх бібліотек.
Розмітка сторінки винесена в ``templates/report.html``; цей модуль лише
готує фрагменти даних і підставляє їх у шаблон (``string.Template``).
"""

from __future__ import annotations

import html
from collections.abc import Sequence
from datetime import datetime
from pathlib import Path
from string import Template
from typing import Callable

from .acts import KYIV, fmt

TEMPLATE = Template((Path(__file__).parent / "templates" / "report.html").read_text(encoding="utf-8"))
TIER_COLORS = ("var(--t0)", "var(--t1)", "var(--t2)", "var(--t3)", "var(--t4)")
TIER_NAMES = ("Р0 Рекомендація", "Р1 Запит", "Р2 Аудит", "Р3 Вимога", "Р4 Примус")
WEIGHT_NAMES = ("Вразливість E", "Загроза T", "Незрілість 1−M", "Історія H")

esc = html.escape


def _thousands(value: float) -> str:
    """Число з пробілом як роздільником тисяч (український стиль)."""
    return f"{value:,.0f}".replace(",", " ")


def _row(*cells: str) -> str:
    return "<tr>" + "".join(cells) + "</tr>"


def _td(content: object, cls: str = "") -> str:
    attr = f' class="{cls}"' if cls else ""
    return f"<td{attr}>{content}</td>"


# --------------------------------------------------------------------------- графіки


def timeline_svg(timeline: list[dict], incidents: list[dict], entities: dict) -> str:
    """Стовпчикова діаграма: кількість суб'єктів на кожному рівні нагляду по днях."""
    if not timeline:
        return ""
    width, height, left, right, top, bottom = 760, 300, 44, 16, 24, 46
    total = sum(timeline[0]["tiers"])
    bar_w = (width - left - right) / len(timeline)

    def y(v: float) -> float:
        return top + (height - top - bottom) * (1 - v / total)

    svg = [f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="Рівні нагляду по днях">']
    for k in range(0, total + 1, max(1, total // 7)):
        svg.append(f'<line x1="{left}" x2="{width - right}" y1="{y(k):.1f}" y2="{y(k):.1f}" class="grid"/>')
        svg.append(f'<text x="{left - 8}" y="{y(k) + 4:.1f}" text-anchor="end" class="ax">{k}</text>')

    for i, day in enumerate(timeline):
        x, stacked = left + i * bar_w, 0
        for tier, count in enumerate(day["tiers"]):
            if count:
                y_top, h = y(stacked + count), y(stacked) - y(stacked + count)
                tip = f"{fmt(day['t'])[:10]}: {TIER_NAMES[tier]} — {count}"
                svg.append(
                    f'<rect x="{x + 1:.1f}" y="{y_top:.1f}" width="{bar_w - 2:.1f}" height="{h:.1f}" '
                    f'fill="{TIER_COLORS[tier]}"><title>{tip}</title></rect>'
                )
            stacked += count
        if day["urgent"]:
            r = 2 + min(5, day["urgent"])
            svg.append(
                f'<circle cx="{x + bar_w / 2:.1f}" cy="{top - 8}" r="{r}" fill="var(--signal)">'
                f"<title>Термінова процедура: {day['urgent']}</title></circle>"
            )
        if i % 5 == 0:
            svg.append(
                f'<text x="{x + bar_w / 2:.1f}" y="{height - bottom + 16}" text-anchor="middle" class="ax">'
                f"{fmt(day['t'])[:5]}</text>"
            )

    t_start, t_end = timeline[0]["t"], timeline[-1]["t"] + 86400
    for inc in incidents:
        x = left + (inc["start"] - t_start) / (t_end - t_start) * (width - left - right)
        name = esc(entities[inc["entity"]]["name"])
        svg.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{top}" y2="{height - bottom}" class="inc"/>')
        svg.append(
            f'<text x="{x:.1f}" y="{height - 8}" text-anchor="middle" class="ax inc-l">{esc(inc["id"][-3:])}'
            f"<title>{esc(inc['id'])}: {name}</title></text>"
        )
    svg.append("</svg>")
    return "".join(svg)


def bar_list(
    rows: Sequence[tuple[str, float, str]],
    value_fmt: Callable[[float], str],
    color: str = "var(--accent)",
) -> str:
    """Горизонтальні смуги: підпис, смуга, значення і примітка під рядком."""
    top_value = max((r[1] for r in rows), default=1) or 1
    out = ['<div class="bars">']
    for label, value, note in rows:
        width = 100 * value / top_value
        out.append(
            f'<div class="b-label">{esc(label)}</div>'
            f'<div class="b-track"><i style="width:{width:.1f}%;background:{color}"></i></div>'
            f'<div class="b-val num">{value_fmt(value)}</div>'
            f'<div class="b-note">{esc(note)}</div>'
        )
    out.append("</div>")
    return "".join(out)


# --------------------------------------------------------------------------- таблиці


def _incident_rows(incidents: list[dict], entities: dict) -> str:
    rows = []
    for inc in incidents:
        cells = []
        for stage in inc["stages"]:
            if stage["submitted"] is not None:
                late = stage["submitted"] > stage["due"]
                text = fmt(stage["submitted"]) + (" · запізно" if late else "")
                cells.append(_td(text, "late" if late else "ok"))
            elif stage["breached"]:
                cells.append(_td("не подано", "late"))
            else:
                cells.append(_td("очікується"))
        hub_note = " · вплив на хаб" if inc["hub_wide"] else ""
        who = f'<b>{esc(entities[inc["entity"]]["name"])}</b><div class="sub">{esc(inc["title"])}{hub_note}</div>'
        rows.append(_row(_td(esc(inc["id"]), "mono"), _td(who), _td(esc(inc["supplier"])), *cells))
    return "".join(rows)


def _calibration_block(calib: dict) -> str:
    if calib.get("status") != "ok":
        return f"<p>{esc(calib.get('message', ''))}</p>"
    rows = "".join(
        _row(
            _td(name),
            _td(f"{cur:.2f}", "num"),
            _td(f"<b>{new:.2f}</b>", "num"),
            _td(f"{lo:.2f}–{hi:.2f}", "num"),
            _td(f"{coef:+.2f}", "num"),
        )
        for name, cur, new, lo, hi, coef in zip(
            WEIGHT_NAMES,
            calib["current_weights"],
            calib["suggested_weights"],
            calib["ci90_low"],
            calib["ci90_high"],
            calib["coefficients"],
        )
    )
    head = "".join(f"<th>{h}</th>" for h in ("Складова", "Поточна вага", "Запропонована", "90% інтервал", "Коефіцієнт"))
    summary = (
        f"Аудитів: {calib['n']}, підтверджено порушень: {calib['confirmed']}. "
        f"AUC поточного індексу {calib['auc_current']:.3f}, каліброваного {calib['auc_suggested']:.3f}."
    )
    return (
        f"<p>{esc(calib['message'])}</p>"
        f'<div class="tablewrap"><table><thead><tr>{head}</tr></thead><tbody>{rows}</tbody></table></div>'
        f'<p class="sub">{summary}</p>'
    )


def _act_rows(acts: list[dict], entities: dict, limit: int = 40) -> str:
    return "".join(
        _row(
            _td(esc(a["number"]), "mono"),
            _td(fmt(a["t"])),
            _td(esc(entities[a["entity"]]["name"])),
            _td(esc(a["kind"])),
            _td(fmt(a["deadline"])),
            _td("так" if a["requires_human_review"] else "—"),
        )
        for a in acts[:limit]
    )


def _registry_rows(entities: list[dict]) -> str:
    rows = []
    for x in sorted(entities, key=lambda item: -item["assessment"]["score"]):
        tier = x["assessment"]["tier"]
        cls = "суттєвий" if x["class"] == "essential" else "важливий"
        pill = f'<span class="pill" style="--c:{TIER_COLORS[tier]}">{TIER_NAMES[tier]}</span>'
        rows.append(
            _row(
                _td(f'<b>{esc(x["name"])}</b><div class="sub">{esc(x["kind"])}</div>'),
                _td(cls),
                _td(x["assessment"]["score"], "num"),
                _td(pill),
                _td(f"{x['E']:.2f} / {x['M']:.2f}", "num"),
                _td(f"{x['audits_confirmed']} / {x['audits_cleared']}", "num"),
            )
        )
    return "".join(rows)


# --------------------------------------------------------------------------- збирання


def build(result, cascade: dict, calib: dict, transparency: dict, perf: dict) -> str:
    """Повертає готовий HTML звіту за результатами запуску конвеєра."""
    snap = result.snapshot
    entities = {x["id"]: x for x in snap["entities"]}

    def count(kind: str) -> int:
        return sum(1 for d in result.decisions if d["type"] == kind)

    kpis = (
        ("Подій оброблено", _thousands(result.events_in)),
        ("Ескалацій", count("escalation")),
        ("Термінових процедур", count("urgent_open")),
        ("Порушень строків NIS2", count("deadline_breach")),
        ("Проєктів актів", len(result.acts)),
        ("З обов'язковою перевіркою", sum(1 for a in result.acts if a["requires_human_review"])),
    )
    kpi_html = "".join(
        f'<div class="kpi"><span class="label">{esc(k)}</span><span class="n num">{v}</span></div>' for k, v in kpis
    )

    cascade_html = bar_list(
        [
            (
                s["label"] + (" · постачальник" if s["type"] == "supplier" else ""),
                s["expected_impact"],
                f"хабів: {s['hubs_reached']}, P(зупинка хабу) {s['p_hub_disruption']:.0%}",
            )
            for s in cascade["seeds"][:8]
        ],
        lambda v: f"{v:.2f}",
    )
    mitigation_html = bar_list(
        [
            (m["name"], m["reduction_pct"], f"ризик {m['risk_before']:.2f} → {m['risk_after']:.2f}")
            for m in cascade["mitigation"][:6]
        ],
        lambda v: f"−{v:.1f}%",
        color="var(--ok)",
    )

    legend = "".join(f'<span><i style="background:{c}"></i>{n}</span>' for c, n in zip(TIER_COLORS, TIER_NAMES))
    legend += '<span><i style="background:var(--signal);border-radius:50%"></i>Термінова процедура</span>'
    period = ""
    if result.timeline:
        period = f"{fmt(result.timeline[0]['t'])[:10]} – {fmt(result.timeline[-1]['t'])[:10]}"

    return TEMPLATE.substitute(
        period=period,
        generated=datetime.now(KYIV).strftime("%d.%m.%Y %H:%M"),
        kpis=kpi_html,
        timeline=timeline_svg(result.timeline, snap["incidents"], entities),
        legend=legend,
        incidents=_incident_rows(snap["incidents"], entities),
        cascade=cascade_html,
        mitigation=mitigation_html,
        sims=_thousands(cascade["simulations"]),
        csec=f"{cascade['seconds']:.2f}",
        systemic=f"{cascade['systemic_risk']:.2f}",
        calib=_calibration_block(calib),
        acts=_act_rows(result.acts, entities),
        final=_registry_rows(snap["entities"]),
        eps=_thousands(perf.get("events_per_second", 0)),
        backend=esc(perf.get("backend", "")),
        tr_esc=transparency["escalations"],
        tr_aud=transparency["audits"],
        tr_clr=transparency["audits_cleared"],
        n_acts=len(result.acts),
    )
