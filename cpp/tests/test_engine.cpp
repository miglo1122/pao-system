// Модульні тести ядра PAO (без зовнішніх фреймворків).
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "pao/cascade.hpp"
#include "pao/engine.hpp"

using namespace pao;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                             \
  do {                                                                          \
    ++checks;                                                                   \
    if (!(cond)) {                                                              \
      ++failures;                                                               \
      std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
    }                                                                           \
  } while (0)

static const double T0 = 1792994400;  // понеділок, 26.10.2026 06:00 UTC

static Value registry(const std::string& entities_json) {
  return json::parse(R"({"t0":1792994400,"threats":{"rail":0.6,"sea":0.5,"road":0.3},
    "hubs":[{"id":"h","name":"Хаб","coordinator":"c"},{"id":"g","name":"Інший","coordinator":"z"}],
    "entities":)" + entities_json +
                     "}");
}

static bool has_type(const std::vector<Value>& ds, const std::string& t, const std::string& ent = "") {
  for (const auto& d : ds)
    if (d.str("type") == t && (ent.empty() || d.str("entity") == ent)) return true;
  return false;
}

static void test_json() {
  Value v = json::parse(R"({"назва":"Порт «Південь»\nрядок","u":"і","n":[1,2.5,-3e2],"b":true,"z":null})");
  CHECK(v.str("назва") == "Порт «Південь»\nрядок");
  CHECK(v.str("u") == "і");
  CHECK(v["n"].as_array().size() == 3);
  CHECK(std::fabs(v["n"].as_array()[2].as_number() + 300) < 1e-9);
  Value again = json::parse(v.dump());
  CHECK(again.dump() == v.dump());
  bool threw = false;
  try {
    json::parse("{\"a\":}");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

static void test_score_formula() {
  Engine e(Config::from_json(Value(json::Object{})));
  e.load_registry(registry(R"([{"id":"a","hub":"h","mode":"rail","E":0.5,"M":0.5,"H0":0}])"));
  Assessment a = e.assess(e.entities()[0]);
  // 100 · 1.0 · (0.30·0.5 + 0.25·0.6 + 0.30·0.5 + 0.15·0) = 45
  CHECK(a.score == 45);
  CHECK(a.tier == 1);
}

static void test_coordinator_and_important() {
  Engine e(Config::from_json(Value(json::Object{})));
  e.load_registry(registry(R"([
     {"id":"c","hub":"h","mode":"rail","E":0.5,"M":0.5},
     {"id":"i","hub":"h","mode":"rail","class":"important","E":0.9,"M":0.1}])"));
  Assessment c = e.assess(e.entities()[0]);
  CHECK(c.score == 50);  // 45 · 1.1 = 49.5 → 50, координатор хабу
  Assessment i = e.assess(e.entities()[1]);
  CHECK(i.score >= 30);
  CHECK(i.tier == 0);  // ex post: без ознак порушень ескалації немає
  CHECK(!i.indication);
}

static void test_missed_report() {
  Engine e(Config::from_json(Value(json::Object{})));
  e.load_registry(registry(R"([{"id":"a","hub":"h","mode":"road","E":0.1,"M":0.95}])"));
  e.process(Value(json::Object{{"type", "tick"}, {"t", T0}}));
  CHECK(e.entities()[0].tier == 0);
  e.process(Value(
      json::Object{{"type", "incident"}, {"t", T0}, {"id", "X"}, {"entity", "a"}, {"significant", false}}));
  auto ds = e.process(Value(json::Object{{"type", "tick"}, {"t", T0 + 25 * 3600}}));
  CHECK(has_type(ds, "deadline_breach", "a"));
  CHECK(has_type(ds, "escalation", "a"));
  CHECK(e.entities()[0].tier >= 1);
  auto again = e.process(Value(json::Object{{"type", "tick"}, {"t", T0 + 26 * 3600}}));
  CHECK(!has_type(again, "deadline_breach"));  // порушення фіксується один раз
}

static void test_supplier_contagion() {
  Engine e(Config::from_json(Value(json::Object{})));
  e.load_registry(registry(R"([
     {"id":"a","hub":"h","mode":"road","suppliers":["S"],"E":0.2,"M":0.8},
     {"id":"b","hub":"g","mode":"road","suppliers":["S"],"E":0.2,"M":0.8},
     {"id":"d","hub":"g","mode":"road","suppliers":["Q"],"E":0.2,"M":0.8}])"));
  e.process(Value(json::Object{{"type", "tick"}, {"t", T0}}));
  auto ds = e.process(Value(json::Object{{"type", "incident"},
                                         {"t", T0},
                                         {"id", "X"},
                                         {"entity", "a"},
                                         {"supplier", "S"},
                                         {"early_warning", true}}));
  CHECK(has_type(ds, "urgent_open", "a"));
  CHECK(has_type(ds, "urgent_open", "b"));  // інший хаб, той самий постачальник
  CHECK(!has_type(ds, "urgent_open", "d"));
  CHECK(std::fabs(e.assess(e.entities()[1]).T - 0.85) < 1e-9);
  for (const auto& d : ds)
    if (d.str("type") == "urgent_open") CHECK(d["act"].flag("requires_human_review"));
}

static void test_rejections_and_working_days() {
  Engine e(Config::from_json(Value(json::Object{})));
  e.load_registry(registry(R"([{"id":"a","hub":"h","mode":"road"}])"));
  CHECK(has_type(e.process(Value(json::Object{{"type", "telemetry"}, {"t", T0}, {"entity", "nope"}})),
                 "rejected_event"));
  e.process(Value(json::Object{{"type", "incident"}, {"t", T0}, {"id", "X"}, {"entity", "a"}}));
  CHECK(has_type(e.process(Value(json::Object{{"type", "close"}, {"t", T0}, {"incident", "X"}})),
                 "rejected_event"));
  double friday = T0 + 4 * 86400;
  CHECK(std::fabs(add_working_days(friday, 1) - (friday + 3 * 86400)) < 1);  // пт → пн
  CHECK(std::fabs(add_working_days(T0, 5) - (T0 + 7 * 86400)) < 1);          // пн + 5 р.д. → пн
}

static void test_cascade() {
  std::vector<Entity> es;
  for (int i = 0; i < 6; ++i) {
    Entity e;
    e.id = "e" + std::to_string(i);
    e.name = e.id;
    e.hub = i < 3 ? "h" : "g";
    e.M = 0.3;
    if (i != 5) e.suppliers.push_back("BIG");  // спільний постачальник у двох хабах
    if (i == 5) e.suppliers.push_back("SMALL");
    es.push_back(e);
  }
  CascadeParams p;
  p.runs = 4000;
  p.threads = 2;
  auto r1 = run_cascade(es, {{"h", "e0"}, {"g", "e3"}}, p, true);
  auto r2 = run_cascade(es, {{"h", "e0"}, {"g", "e3"}}, p, true);
  CHECK(r1.to_json()["seeds"].dump() == r2.to_json()["seeds"].dump());  // відтворюваність
  double big = 0, small = 0;
  for (const auto& s : r1.seeds) {
    if (s.node == "sup:BIG") {
      big = s.expected_impact;
      CHECK(s.hubs_reached == 2);
    }
    if (s.node == "sup:SMALL") small = s.expected_impact;
  }
  CHECK(big > 2 * small);
  CHECK(!r1.mitigation.empty());
  CHECK(r1.mitigation.front().reduction > 0);
}

static void test_json_edge_cases() {
  // Сурогатна пара UTF-16 (емодзі) та керівні символи мають пройти туди й назад.
  Value v = json::parse(R"({"s":"\ud83d\ude86 \u0007","e":1.5e3,"neg":-0.25,"arr":[],"obj":{}})");
  CHECK(v.str("s") == "\xF0\x9F\x9A\x86 \x07");
  CHECK(json::parse(v.dump()).str("s") == v.str("s"));
  CHECK(v.num("e") == 1500);
  CHECK(v["arr"].as_array().empty());
  CHECK(v["obj"].as_object().empty());
  CHECK(Value(0.125).dump() == "0.125");
  CHECK(Value(std::nan("")).dump() == "null");
  for (const char* bad : {"[1,", "\"abc", "{\"a\" 1}", "tru", "[1 2]", "\"\\q\"", "{} x"}) {
    bool threw = false;
    try {
      json::parse(bad);
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }
}

static void test_config_validation() {
  Config c =
      Config::from_json(json::parse(R"({"weights":{"E":2,"T":1,"M":1,"H":0},"thresholds":[10,20,30,40]})"));
  CHECK(std::fabs(c.wE - 0.5) < 1e-12);
  CHECK(c.thresholds[3] == 40);
  bool threw = false;
  try {
    Config::from_json(json::parse(R"({"weights":{"E":0,"T":0,"M":0,"H":0}})"));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

static void test_full_incident_lifecycle() {
  Engine e(Config::from_json(Value(json::Object{})));
  e.load_registry(registry(R"([{"id":"a","hub":"h","mode":"sea","E":0.4,"M":0.6}])"));
  auto run = [&](const std::string& j) { return e.process(json::parse(j)); };
  run(R"({"type":"tick","t":1792994400})");
  CHECK(has_type(run(R"({"type":"incident","t":1792994400,"id":"I","entity":"a"})"), "incident_registered"));
  auto early = run(R"({"type":"report","t":1793001600,"incident":"I","stage":"early"})");
  CHECK(has_type(early, "report_received"));
  CHECK(
      has_type(run(R"({"type":"report","t":1793001700,"incident":"I","stage":"early"})"), "rejected_event"));
  CHECK(
      has_type(run(R"({"type":"report","t":1793001800,"incident":"I","stage":"bogus"})"), "rejected_event"));
  run(R"({"type":"report","t":1793100000,"incident":"I","stage":"notification"})");
  run(R"({"type":"report","t":1793500000,"incident":"I","stage":"final"})");
  auto closed = run(R"({"type":"close","t":1793500100,"incident":"I"})");
  CHECK(has_type(closed, "urgent_close", "a"));
  CHECK(has_type(run(R"({"type":"tick","t":1793000000})"), "late_event"));  // годинник не йде назад
  CHECK(has_type(run(R"({"type":"audit","t":1793600000,"entity":"a","outcome":"confirmed","M":0.9})"),
                 "audit_feedback"));
  CHECK(std::fabs(e.entities()[0].M - 0.9) < 1e-12);
  CHECK(has_type(run(R"({"type":"audit","t":1793600001,"entity":"a","outcome":"maybe"})"), "rejected_event"));
  CHECK(has_type(run(R"({"type":"unknown","t":1793600002})"), "rejected_event"));
  run(R"({"type":"register","t":1793600003,"entity":{"id":"b","hub":"h","mode":"road"}})");
  CHECK(e.entities().size() == 2);
  CHECK(has_type(run(R"({"type":"register","t":1793600004,"entity":{"id":"b"}})"), "rejected_event"));
  run(R"({"type":"threat","t":1793600005,"mode":"road","level":0.9})");
  CHECK(std::fabs(e.assess(e.entities()[1]).T - 0.9) < 1e-12);
  Value snap = e.snapshot();
  CHECK(snap["incidents"].as_array().size() == 1);
  CHECK(snap["incidents"].as_array()[0].flag("closed"));
}

int main() {
  std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"json", test_json},
      {"формула індексу", test_score_formula},
      {"координатор і ex post", test_coordinator_and_important},
      {"прострочений звіт", test_missed_report},
      {"зараження через постачальника", test_supplier_contagion},
      {"відхилення подій, робочі дні", test_rejections_and_working_days},
      {"каскадне моделювання", test_cascade},
      {"json: граничні випадки", test_json_edge_cases},
      {"конфігурація", test_config_validation},
      {"повний життєвий цикл інциденту", test_full_incident_lifecycle}};
  for (auto& [name, f] : tests) {
    int before = failures;
    f();
    std::cout << (failures == before ? "  ok   " : "  FAIL ") << name << "\n";
  }
  std::cout << checks << " перевірок, " << failures << " помилок\n";
  return failures ? 1 : 0;
}
