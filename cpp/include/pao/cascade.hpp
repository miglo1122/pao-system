// Моделювання каскадних кібератак у транспортно-логістичній мережі методом Монте-Карло.
//
// Граф: вузли — суб'єкти нагляду та їхні ІТ-постачальники; ребра — шляхи поширення
// компрометації: постачальник → клієнт (оновлення, віддалений доступ), клієнт →
// постачальник (спільні облікові дані), учасник хабу ↔ учасник хабу (спільні
// інтерфейси портової спільноти), координатор хабу → учасники.
// Ймовірність передачі по ребру зменшується зі зрілістю контролів приймача: p·(1 − M).
//
// Результат для наглядача: (1) які постачальники та суб'єкти є системними точками
// відмови, що зачіпають кілька хабів; (2) куди спрямувати наступний аудит, щоб
// найбільше знизити очікувані системні втрати (граничний ефект +ΔM).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "pao/engine.hpp"

namespace pao {

struct CascadeParams {
  int runs = 20000;          // симуляцій на кожен вузол-джерело
  uint64_t seed = 20260324;  // детермінованість для відтворюваності
  int threads = 0;           // 0 = усі ядра
  double p_supplier_to_client = 0.55;
  double p_client_to_supplier = 0.10;
  double p_hub_peer = 0.12;
  double p_coordinator = 0.30;
  double hub_disruption_share = 0.5;  // хаб зупинено, якщо уражено ≥ частки учасників або координатора
  double mitigation_delta_M = 0.20;  // гіпотетичне підвищення зрілості для аналізу «що, як»
  static CascadeParams from_json(const Value& v);
};

struct SeedResult {
  std::string node, label, type;  // type: "supplier" | "entity"
  double expected_impact = 0;  // очікувана зважена кількість уражених суб'єктів
  double expected_affected = 0;
  double p_hub_disruption = 0;  // ймовірність зупинки хоча б одного хабу
  double expected_hubs_disrupted = 0;
  int hubs_reached = 0;  // кількість різних хабів, яких досягає каскад (p > 1%)
  std::vector<std::pair<std::string, double>> top_affected;
};

struct MitigationResult {
  std::string entity, name;
  double risk_before = 0, risk_after = 0, reduction = 0, reduction_pct = 0;
};

struct CascadeReport {
  std::vector<SeedResult> seeds;             // відсортовано за expected_impact
  std::vector<MitigationResult> mitigation;  // відсортовано за reduction
  double systemic_risk = 0;  // середній очікуваний вплив по всіх джерелах
  long long simulations = 0;
  double seconds = 0;
  Value to_json() const;
};

CascadeReport run_cascade(const std::vector<Entity>& entities,
                          const std::vector<std::pair<std::string, std::string>>& hub_coordinators,
                          const CascadeParams& p, bool with_mitigation = true);

}  // namespace pao
