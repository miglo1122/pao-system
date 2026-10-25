// Реалізація рушія правил PAO.
//
// Порядок обробки кожної події (Engine::process):
//   1. годинник рушія переводиться на час події (назад не повертається);
//   2. apply()          — зміна стану відповідно до типу події;
//   3. check_deadlines() — фіксація прострочених етапів звітування (ст. 23 NIS2);
//   4. reevaluate()     — перерахунок індексу і рівнів, рішення про ескалацію
//                         та термінову процедуру з проєктами актів.

#include "pao/engine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pao {

namespace {
constexpr double HOUR = 3600.0, DAY = 86400.0;
constexpr const char* STAGE_KEYS[3] = {"early", "notification", "final"};
constexpr const char* STAGE_NAMES[3] = {"раннє попередження", "повідомлення про інцидент", "остаточний звіт"};
double clamp01(double v) {
  return std::max(0.0, std::min(1.0, v));
}

struct TierSpec {
  const char* name;
  const char* measure;
  const char* act;
  const char* deadline_label;
  const char* rights;
};
const TierSpec TIERS[5] = {
    {"Р0 Рекомендація", "Автоматичне повідомлення з рекомендованими заходами",
     "Інформаційне повідомлення (рекомендація)", "Підтвердження отримання",
     "Інформаційний акт; не є обов'язковим"},
    {"Р1 Запит інформації", "Запит доказів вжитих заходів або відсутнього звіту",
     "Запит про надання інформації", "Відповідь суб'єкта", "Право надати пояснення"},
    {"Р2 Цільовий аудит",
     "Безпекове сканування або дистанційний аудит за опублікованими критеріями (ст. 32 NIS2)",
     "Рішення про проведення цільового дистанційного аудиту", "Початок аудиту",
     "Право бути заслуханим щодо проєкту висновків"},
    {"Р3 Обов'язкова вимога",
     "Розпорядження про усунення недоліків із планом; можливий моніторинговий офіцер",
     "Обов'язкова до виконання вимога про усунення недоліків", "Подання плану усунення",
     "Обґрунтований адміністративний акт; адміністративне та судове оскарження"},
    {"Р4 Примус", "Виїзна перевірка; штраф від обороту; тимчасове відсторонення керівника (суттєві суб'єкти)",
     "Рішення про виїзну перевірку та заходи впливу", "Початок виїзної перевірки",
     "Повні процедурні гарантії; судовий контроль"}};

double tier_deadline(int tier, double since) {
  switch (tier) {
    case 0:
      return since + 24 * HOUR;
    case 1:
      return since + 72 * HOUR;
    case 2:
      return add_working_days(since, 5);
    default:
      return add_working_days(since, 10);
  }
}

std::vector<std::string> string_list(const Value& v) {
  std::vector<std::string> out;
  if (v.is_array())
    for (const auto& x : v.as_array())
      if (x.is_string()) out.push_back(x.as_string());
  return out;
}
Value to_array(const std::vector<std::string>& xs) {
  json::Array a;
  for (const auto& x : xs) a.emplace_back(x);
  return Value(std::move(a));
}
}  // namespace

const char* tier_name(int tier) {
  return (tier >= 0 && tier <= 4) ? TIERS[tier].name : "—";
}

double add_working_days(double t, int days) {
  int added = 0;
  while (added < days) {
    t += DAY;
    // 1970-01-01 був четвергом: (днів + 4) % 7 дає 0 = неділя
    long long d = static_cast<long long>(std::floor(t / DAY));
    int wd = static_cast<int>(((d + 4) % 7 + 7) % 7);
    if (wd != 0 && wd != 6) ++added;
  }
  return t;
}

// ---------------- Config ----------------
Config Config::from_json(const Value& v) {
  Config c;
  if (!v.is_object()) return c;
  const Value& w = v["weights"];
  c.wE = w.num("E", c.wE);
  c.wT = w.num("T", c.wT);
  c.wM = w.num("M", c.wM);
  c.wH = w.num("H", c.wH);
  if (v["thresholds"].is_array() && v["thresholds"].as_array().size() == 4)
    for (int i = 0; i < 4; ++i) c.thresholds[i] = v["thresholds"].as_array()[i].as_number();
  c.c_essential = v.num("c_essential", c.c_essential);
  c.c_important = v.num("c_important", c.c_important);
  c.coordinator_mult = v.num("coordinator_mult", c.coordinator_mult);
  c.supplier_compromise_threat = v.num("supplier_compromise_threat", c.supplier_compromise_threat);
  c.hub_peer_threat = v.num("hub_peer_threat", c.hub_peer_threat);
  c.h_per_significant = v.num("h_per_significant", c.h_per_significant);
  c.h_per_overdue = v.num("h_per_overdue", c.h_per_overdue);
  double sum = c.wE + c.wT + c.wM + c.wH;
  if (sum <= 0) throw std::invalid_argument("сума ваг індексу має бути додатною");
  c.wE /= sum;
  c.wT /= sum;
  c.wM /= sum;
  c.wH /= sum;
  return c;
}

Value Config::to_json() const {
  return Value(json::Object{
      {"weights", Value(json::Object{{"E", wE}, {"T", wT}, {"M", wM}, {"H", wH}})},
      {"thresholds", Value(json::Array{thresholds[0], thresholds[1], thresholds[2], thresholds[3]})},
      {"c_essential", c_essential},
      {"c_important", c_important},
      {"coordinator_mult", coordinator_mult},
      {"supplier_compromise_threat", supplier_compromise_threat},
      {"hub_peer_threat", hub_peer_threat}});
}

// ---------------- Entities ----------------
Entity entity_from_json(const Value& v) {
  Entity e;
  e.id = v.str("id");
  if (e.id.empty()) throw std::invalid_argument("суб'єкт без id");
  e.name = v.str("name", e.id);
  e.kind = v.str("kind");
  e.mode = v.str("mode", "road");
  e.hub = v.str("hub");
  e.cls = v.str("class", "essential") == "important" ? Cls::Important : Cls::Essential;
  e.suppliers = string_list(v["suppliers"]);
  e.E = clamp01(v.num("E", 0.5));
  e.M = clamp01(v.num("M", 0.5));
  e.H0 = clamp01(v.num("H0", 0.0));
  return e;
}

Value entity_to_json(const Entity& e) {
  return Value(json::Object{{"id", e.id},
                            {"name", e.name},
                            {"kind", e.kind},
                            {"mode", e.mode},
                            {"hub", e.hub},
                            {"class", e.cls == Cls::Essential ? "essential" : "important"},
                            {"suppliers", to_array(e.suppliers)},
                            {"E", e.E},
                            {"M", e.M},
                            {"H0", e.H0},
                            {"tier", e.tier},
                            {"tier_since", e.tier_since},
                            {"urgent", e.urgent},
                            {"audits_confirmed", e.audits_confirmed},
                            {"audits_cleared", e.audits_cleared}});
}

Value Assessment::to_json() const {
  return Value(
      json::Object{{"score", score},
                   {"tier", tier},
                   {"tier_name", tier_name(tier)},
                   {"components", Value(json::Object{{"E", E}, {"T", T}, {"M", M}, {"H", H}, {"C", C}})},
                   {"contrib", Value(json::Array{contrib[0], contrib[1], contrib[2], contrib[3]})},
                   {"indication", indication},
                   {"overdue_stages", overdue},
                   {"reasons", to_array(reasons)},
                   {"urgent_reasons", to_array(urgent_reasons)}});
}

// ---------------- Engine ----------------
Engine::Engine(Config cfg) : cfg_(cfg) {
  threats_ = {{"sea", 0.5}, {"water", 0.4}, {"rail", 0.5}, {"road", 0.3}, {"air", 0.4}};
}

void Engine::load_registry(const Value& r) {
  hubs_.clear();
  entities_.clear();
  incidents_.clear();
  if (r["hubs"].is_array())
    for (const auto& h : r["hubs"].as_array())
      hubs_.push_back({h.str("id"), h.str("name", h.str("id")), h.str("coordinator")});
  if (r["entities"].is_array())
    for (const auto& e : r["entities"].as_array()) entities_.push_back(entity_from_json(e));
  if (r["threats"].is_object())
    for (const auto& [k, v] : r["threats"].as_object())
      if (v.is_number()) threats_[k] = clamp01(v.as_number());
  now_ = r.num("t0", now_);
}

Entity* Engine::find_entity(const std::string& id) {
  for (auto& e : entities_)
    if (e.id == id) return &e;
  return nullptr;
}
const Entity* Engine::find_entity(const std::string& id) const {
  for (const auto& e : entities_)
    if (e.id == id) return &e;
  return nullptr;
}
const Hub* Engine::find_hub(const std::string& id) const {
  for (const auto& h : hubs_)
    if (h.id == id) return &h;
  return nullptr;
}
Incident* Engine::find_incident(const std::string& id) {
  for (auto& i : incidents_)
    if (i.id == id) return &i;
  return nullptr;
}
double Engine::stage_due(const Incident& inc, int s) const {
  return inc.start + cfg_.stage_hours[s] * HOUR;
}
int Engine::overdue_stages(const Incident& inc) const {
  if (inc.closed) return 0;
  int n = 0;
  for (int s = 0; s < 3; ++s)
    if (inc.submitted[s] < 0 && now_ > stage_due(inc, s)) ++n;
  return n;
}

Assessment Engine::assess(const Entity& e) const {
  Assessment a;
  std::set<std::string> compromised;
  for (const auto& i : incidents_)
    if (!i.closed && !i.supplier.empty()) compromised.insert(i.supplier);

  int own_sig = 0;
  for (const auto& i : incidents_) {
    if (i.closed) continue;
    if (i.entity == e.id) {
      if (i.significant) {
        ++own_sig;
        a.urgent_reasons.push_back("власний суттєвий інцидент " + i.id);
      }
      a.overdue += overdue_stages(i);
      continue;
    }
    const Entity* other = find_entity(i.entity);
    if (other && i.significant && i.hub_wide && other->hub == e.hub)
      a.urgent_reasons.push_back("інцидент із впливом на хаб " + i.id + " (" + other->name + ")");
    if (!i.supplier.empty() &&
        std::find(e.suppliers.begin(), e.suppliers.end(), i.supplier) != e.suppliers.end())
      a.urgent_reasons.push_back("спільний скомпрометований постачальник " + i.supplier + " (" + i.id + ")");
  }

  std::vector<std::string> sup_hit;
  for (const auto& s : e.suppliers)
    if (compromised.count(s)) sup_hit.push_back(s);
  bool hub_hit = false;
  for (const auto& r : a.urgent_reasons)
    if (r.rfind("інцидент із впливом на хаб", 0) == 0) hub_hit = true;

  auto it = threats_.find(e.mode);
  a.T = it == threats_.end() ? 0.3 : it->second;
  if (!sup_hit.empty()) a.T = std::max(a.T, cfg_.supplier_compromise_threat);
  if (hub_hit) a.T = std::max(a.T, cfg_.hub_peer_threat);
  a.E = e.E;
  a.M = e.M;
  a.H = clamp01(e.H0 + cfg_.h_per_significant * own_sig + cfg_.h_per_overdue * a.overdue);

  const Hub* h = find_hub(e.hub);
  a.C = (e.cls == Cls::Essential ? cfg_.c_essential : cfg_.c_important) *
        (h && h->coordinator == e.id ? cfg_.coordinator_mult : 1.0);
  a.contrib = {cfg_.wE * a.E, cfg_.wT * a.T, cfg_.wM * (1 - a.M), cfg_.wH * a.H};
  double raw = 100.0 * a.C * (a.contrib[0] + a.contrib[1] + a.contrib[2] + a.contrib[3]);
  a.score = static_cast<int>(std::lround(std::min(100.0, raw)));

  int tier = 0;
  for (int k = 0; k < 4; ++k)
    if (a.score >= cfg_.thresholds[k]) tier = k + 1;
  a.reasons.push_back("індекс " + std::to_string(a.score) + " → " + tier_name(tier));

  a.indication = a.overdue > 0 || own_sig > 0 || a.H >= cfg_.h_indication || !sup_hit.empty() || hub_hit;
  if (e.cls == Cls::Important && !a.indication && tier > 0) {
    tier = 0;
    a.reasons.push_back("важливий суб'єкт: нагляд ex post (ст. 33 NIS2), ознак порушень немає");
  }
  if (a.overdue > 0) {
    if (tier < 1) tier = 1;
    a.reasons.push_back("прострочено етапів звітування: " + std::to_string(a.overdue));
  }
  for (const auto& s : sup_hit) a.reasons.push_back("загрозу підвищено через постачальника " + s);
  if (hub_hit) a.reasons.push_back("загрозу підвищено через інцидент у хабі");
  a.tier = tier;
  return a;
}

Value Engine::decision(const std::string& type, const std::string& entity) const {
  json::Object o{{"t", now_}, {"type", type}};
  if (!entity.empty()) o["entity"] = entity;
  return Value(std::move(o));
}

Value Engine::make_act(const Entity& e, const Assessment& a, bool urgent) {
  ++act_seq_;
  const TierSpec& spec = TIERS[a.tier];
  double since = urgent ? e.urgent_since : e.tier_since;
  double deadline = urgent ? since + 24 * HOUR : tier_deadline(a.tier, since);
  json::Array basis{"Закон України «Про основні засади забезпечення кібербезпеки України»",
                    "Закон України «Про критичну інфраструктуру»",
                    "Закон України «Про адміністративну процедуру»",
                    std::string("Директива (ЄС) 2022/2555, ст. ") + (e.cls == Cls::Essential ? "32" : "33") +
                        (urgent ? ", 23" : "") + " (орієнтир гармонізації)"};
  return Value(json::Object{
      {"number", "PAO-" + std::to_string(act_seq_)},
      {"kind", urgent ? "Рішення про застосування тимчасових (забезпечувальних) заходів" : spec.act},
      {"measure",
       urgent ? "Ізоляція спільних інтерфейсів, посилений моніторинг пов'язаних суб'єктів" : spec.measure},
      {"deadline", deadline},
      {"deadline_label", urgent ? "Тимчасові заходи" : spec.deadline_label},
      {"rights", urgent ? "Виняток щодо попереднього заслуховування через невідкладність; "
                          "подальше заслуховування та оскарження"
                        : spec.rights},
      {"requires_human_review", urgent || a.tier >= 3},
      {"legal_basis", Value(std::move(basis))},
      {"assessment", a.to_json()}});
}

void Engine::apply(const Value& ev, std::vector<Value>& out) {
  const std::string type = ev.str("type");
  auto reject = [&](const std::string& why) {
    Value d = decision("rejected_event", ev.str("entity"));
    d.as_object()["reason"] = why;
    d.as_object()["event_type"] = type;
    out.push_back(d);
  };

  if (type == "tick") return;
  if (type == "register") {
    Entity e = entity_from_json(ev["entity"]);
    if (find_entity(e.id)) return reject("суб'єкт уже зареєстровано: " + e.id);
    entities_.push_back(e);
    return;
  }
  if (type == "threat") {
    threats_[ev.str("mode")] = clamp01(ev.num("level", 0.5));
    return;
  }
  if (type == "telemetry") {
    Entity* e = find_entity(ev.str("entity"));
    if (!e) return reject("невідомий суб'єкт");
    if (ev.has("E")) e->E = clamp01(ev.num("E"));
    if (ev.has("M")) e->M = clamp01(ev.num("M"));
    return;
  }
  if (type == "incident") {
    if (!find_entity(ev.str("entity"))) return reject("невідомий суб'єкт");
    Incident i;
    i.id = ev.str("id", "INC-" + std::to_string(incidents_.size() + 1));
    if (find_incident(i.id)) return reject("інцидент уже існує: " + i.id);
    i.entity = ev.str("entity");
    i.title = ev.str("title");
    i.supplier = ev.str("supplier");
    i.significant = ev.flag("significant", true);
    i.hub_wide = ev.flag("hub_wide", false);
    i.start = ev.num("aware_at", ev.num("t", now_));
    if (ev.flag("early_warning", false)) i.submitted[0] = ev.num("t", now_);
    incidents_.push_back(i);
    Value d = decision("incident_registered", i.entity);
    d.as_object()["incident"] = i.id;
    d.as_object()["due"] = Value(json::Array{stage_due(i, 0), stage_due(i, 1), stage_due(i, 2)});
    out.push_back(d);
    return;
  }
  if (type == "report") {
    Incident* i = find_incident(ev.str("incident"));
    if (!i) return reject("невідомий інцидент");
    const std::string st = ev.str("stage");
    int s = st == "early" ? 0 : st == "notification" ? 1 : st == "final" ? 2 : -1;
    if (s < 0) return reject("невідомий етап звітування: " + st);
    if (i->submitted[s] >= 0) return reject("етап уже подано");
    double t = ev.num("t", now_);
    i->submitted[s] = t;
    Value d = decision("report_received", i->entity);
    d.as_object()["incident"] = i->id;
    d.as_object()["stage"] = st;
    d.as_object()["late"] = t > stage_due(*i, s);
    out.push_back(d);
    return;
  }
  if (type == "close") {
    Incident* i = find_incident(ev.str("incident"));
    if (!i) return reject("невідомий інцидент");
    if (i->submitted[2] < 0) return reject("закриття неможливе без остаточного звіту");
    i->closed = true;
    return;
  }
  if (type == "audit") {
    Entity* e = find_entity(ev.str("entity"));
    if (!e) return reject("невідомий суб'єкт");
    const std::string outcome = ev.str("outcome");
    Value d = decision("audit_feedback", e->id);
    d.as_object()["outcome"] = outcome;
    d.as_object()["tier_at_audit"] = e->tier;
    if (outcome == "cleared") {
      ++e->audits_cleared;
      e->M = clamp01(e->M + 0.10);
      e->E = clamp01(e->E - 0.10);
    } else if (outcome == "confirmed") {
      ++e->audits_confirmed;
      e->H0 = clamp01(e->H0 + 0.10);
      if (ev.has("M")) e->M = clamp01(ev.num("M"));
    } else {
      return reject("результат аудиту має бути confirmed або cleared");
    }
    out.push_back(d);
    return;
  }
  reject("невідомий тип події");
}

void Engine::check_deadlines(std::vector<Value>& out) {
  for (auto& i : incidents_) {
    if (i.closed) continue;
    for (int s = 0; s < 3; ++s) {
      if (i.submitted[s] < 0 && !i.breached[s] && now_ > stage_due(i, s)) {
        i.breached[s] = true;
        Value d = decision("deadline_breach", i.entity);
        d.as_object()["incident"] = i.id;
        d.as_object()["stage"] = STAGE_KEYS[s];
        d.as_object()["stage_name"] = STAGE_NAMES[s];
        d.as_object()["due"] = stage_due(i, s);
        out.push_back(d);
      }
    }
  }
}

void Engine::reevaluate(std::vector<Value>& out) {
  for (auto& e : entities_) {
    Assessment a = assess(e);
    if (e.tier < 0) {
      e.tier = a.tier;
      e.tier_since = now_;
      Value d = decision("baseline", e.id);
      d.as_object()["tier"] = a.tier;
      d.as_object()["score"] = a.score;
      out.push_back(d);
    } else if (a.tier != e.tier) {
      bool up = a.tier > e.tier;
      Value d = decision(up ? "escalation" : "deescalation", e.id);
      d.as_object()["from"] = e.tier;
      d.as_object()["to"] = a.tier;
      d.as_object()["score"] = a.score;
      e.tier = a.tier;
      e.tier_since = now_;
      if (up && a.tier >= 1)
        d.as_object()["act"] = make_act(e, a, false);
      else
        d.as_object()["assessment"] = a.to_json();
      out.push_back(d);
    }
    bool urgent = !a.urgent_reasons.empty();
    if (urgent && !e.urgent) {
      e.urgent = true;
      e.urgent_since = now_;
      Value d = decision("urgent_open", e.id);
      d.as_object()["act"] = make_act(e, a, true);
      out.push_back(d);
    } else if (!urgent && e.urgent) {
      e.urgent = false;
      out.push_back(decision("urgent_close", e.id));
    }
  }
}

std::vector<Value> Engine::process(const Value& ev) {
  std::vector<Value> out;
  ++processed_;
  double t = ev.num("t", now_);
  if (t + 1e-9 < now_) {
    Value d = decision("late_event", ev.str("entity"));
    d.as_object()["event_t"] = t;
    out.push_back(d);  // подія з минулого: приймається, годинник не повертається назад
  }
  now_ = std::max(now_, t);
  try {
    apply(ev, out);
  } catch (const std::exception& ex) {
    Value d = decision("rejected_event", ev.str("entity"));
    d.as_object()["reason"] = ex.what();
    out.push_back(d);
  }
  check_deadlines(out);
  reevaluate(out);
  return out;
}

Value Engine::snapshot() const {
  json::Array ents, incs;
  for (const auto& e : entities_) {
    Value v = entity_to_json(e);
    v.as_object()["assessment"] = assess(e).to_json();
    ents.push_back(v);
  }
  for (const auto& i : incidents_) {
    json::Array stages;
    for (int s = 0; s < 3; ++s)
      stages.push_back(Value(json::Object{{"stage", STAGE_KEYS[s]},
                                          {"due", stage_due(i, s)},
                                          {"submitted", i.submitted[s] < 0 ? Value() : Value(i.submitted[s])},
                                          {"breached", i.breached[s]}}));
    incs.push_back(Value(json::Object{{"id", i.id},
                                      {"entity", i.entity},
                                      {"title", i.title},
                                      {"supplier", i.supplier},
                                      {"start", i.start},
                                      {"significant", i.significant},
                                      {"hub_wide", i.hub_wide},
                                      {"closed", i.closed},
                                      {"stages", Value(std::move(stages))}}));
  }
  json::Array hubs;
  for (const auto& h : hubs_)
    hubs.push_back(Value(json::Object{{"id", h.id}, {"name", h.name}, {"coordinator", h.coordinator}}));
  json::Object thr;
  for (const auto& [k, v] : threats_) thr[k] = v;
  return Value(json::Object{{"now", now_},
                            {"events_processed", processed_},
                            {"config", cfg_.to_json()},
                            {"hubs", Value(std::move(hubs))},
                            {"threats", Value(std::move(thr))},
                            {"entities", Value(std::move(ents))},
                            {"incidents", Value(std::move(incs))}});
}

}  // namespace pao
