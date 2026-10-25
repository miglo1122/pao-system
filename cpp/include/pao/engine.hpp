// Ядро моделі проактивного адміністративного нагляду (PAO).
// Перетворює потік сигналів (телеметрія, інциденти, розвідка загроз, звіти)
// на юридично значущі події: зміну рівня нагляду, відкриття термінової
// процедури, порушення строків звітування та вимогу сформувати акт.
#pragma once
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pao/json.hpp"

namespace pao {

using json::Value;

enum class Cls { Essential, Important };

struct Config {
  // Ваги складових індексу (нормуються до суми 1).
  double wE = 0.30, wT = 0.25, wM = 0.30, wH = 0.15;
  std::array<double, 4> thresholds{30, 50, 70, 85};  // межі рівнів 1..4
  double c_essential = 1.0, c_important = 0.6, coordinator_mult = 1.1;
  double supplier_compromise_threat = 0.85;  // T для клієнтів скомпрометованого постачальника
  double hub_peer_threat = 0.70;  // T для учасників хабу з інцидентом впливу на хаб
  double h_per_significant = 0.20, h_per_overdue = 0.25, h_indication = 0.30;
  std::array<double, 3> stage_hours{24, 72, 720};  // ст. 23 NIS2: 24 год, 72 год, 1 місяць
  static Config from_json(const Value& v);
  Value to_json() const;
};

struct Entity {
  std::string id, name, kind, mode, hub;
  Cls cls = Cls::Essential;
  std::vector<std::string> suppliers;
  double E = 0.5, M = 0.5, H0 = 0.0;
  // Стан наглядової справи
  int tier = -1;
  double tier_since = 0;
  bool urgent = false;
  double urgent_since = 0;
  int audits_confirmed = 0, audits_cleared = 0;
};

struct Hub {
  std::string id, name, coordinator;
};

struct Incident {
  std::string id, entity, supplier, title;
  double start = 0;
  bool significant = true, hub_wide = false, closed = false;
  std::array<double, 3> submitted{-1, -1, -1};
  std::array<bool, 3> breached{false, false, false};
};

struct Assessment {
  int score = 0, tier = 0, overdue = 0;
  double E = 0, T = 0, M = 0, H = 0, C = 1;
  std::array<double, 4> contrib{};  // зважені внески в індекс (до множення на 100·C)
  bool indication = false;
  std::vector<std::string> reasons, urgent_reasons;
  Value to_json() const;
};

// Кінцевий строк з урахуванням робочих днів (сб, нд не рахуються; під час
// воєнного стану святкові дні не переносяться, тому окремо не враховані).
double add_working_days(double epoch_seconds, int days);
const char* tier_name(int tier);

class Engine {
 public:
  explicit Engine(Config cfg = {});

  void load_registry(const Value& registry);  // {"hubs":[...],"entities":[...],"threats":{...}}
  std::vector<Value> process(const Value& event);
  Assessment assess(const Entity& e) const;
  Value snapshot() const;

  double now() const { return now_; }
  const std::vector<Entity>& entities() const { return entities_; }
  const Config& config() const { return cfg_; }
  size_t events_processed() const { return processed_; }
  std::vector<std::pair<std::string, std::string>> hub_coordinators() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& h : hubs_) out.emplace_back(h.id, h.coordinator);
    return out;
  }

 private:
  Config cfg_;
  double now_ = 0;
  size_t processed_ = 0;
  int act_seq_ = 0;
  std::vector<Entity> entities_;
  std::vector<Hub> hubs_;
  std::vector<Incident> incidents_;
  std::map<std::string, double> threats_;

  Entity* find_entity(const std::string& id);
  const Entity* find_entity(const std::string& id) const;
  const Hub* find_hub(const std::string& id) const;
  Incident* find_incident(const std::string& id);
  int overdue_stages(const Incident& inc) const;
  double stage_due(const Incident& inc, int stage) const;

  void apply(const Value& ev, std::vector<Value>& out);
  void check_deadlines(std::vector<Value>& out);
  void reevaluate(std::vector<Value>& out);
  Value make_act(const Entity& e, const Assessment& a, bool urgent);
  Value decision(const std::string& type, const std::string& entity) const;
};

Entity entity_from_json(const Value& v);
Value entity_to_json(const Entity& e);

}  // namespace pao
