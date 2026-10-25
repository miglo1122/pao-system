// Реалізація моделювання каскадних атак (модель незалежних каскадів, Монте-Карло).
// Опис моделі та формули ймовірностей наведено в docs/METHODOLOGY.md, розділ 5.

#include "pao/cascade.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <thread>

namespace pao {

CascadeParams CascadeParams::from_json(const Value& v) {
  CascadeParams p;
  if (!v.is_object()) return p;
  p.runs = static_cast<int>(v.num("runs", p.runs));
  p.seed = static_cast<uint64_t>(v.num("seed", static_cast<double>(p.seed)));
  p.threads = static_cast<int>(v.num("threads", p.threads));
  p.p_supplier_to_client = v.num("p_supplier_to_client", p.p_supplier_to_client);
  p.p_client_to_supplier = v.num("p_client_to_supplier", p.p_client_to_supplier);
  p.p_hub_peer = v.num("p_hub_peer", p.p_hub_peer);
  p.p_coordinator = v.num("p_coordinator", p.p_coordinator);
  p.hub_disruption_share = v.num("hub_disruption_share", p.hub_disruption_share);
  p.mitigation_delta_M = v.num("mitigation_delta_M", p.mitigation_delta_M);
  return p;
}

namespace {

struct Edge {
  int to;
  double p;
};

struct Graph {
  int n_entities = 0;
  std::vector<std::string> ids, labels;
  std::vector<std::vector<Edge>> adj;
  std::vector<double> weight;  // внесок ураженого вузла у системні втрати
  std::vector<int> hub_of;  // індекс хабу для суб'єкта, -1 для постачальника
  std::vector<int> hub_size;
  std::vector<int> hub_coord;  // вузол-координатор хабу або -1
  int n_hubs = 0;
};

Graph build(const std::vector<Entity>& es, const std::map<std::string, std::string>& coord,
            const CascadeParams& p, int boosted = -1) {
  Graph g;
  g.n_entities = static_cast<int>(es.size());
  std::map<std::string, int> hub_idx, sup_idx;
  for (const auto& e : es)
    if (!hub_idx.count(e.hub)) hub_idx[e.hub] = static_cast<int>(hub_idx.size());
  g.n_hubs = static_cast<int>(hub_idx.size());
  g.hub_size.assign(g.n_hubs, 0);
  g.hub_coord.assign(g.n_hubs, -1);

  std::vector<double> M(es.size());
  for (size_t i = 0; i < es.size(); ++i) {
    const Entity& e = es[i];
    M[i] = std::min(1.0, e.M + (static_cast<int>(i) == boosted ? p.mitigation_delta_M : 0.0));
    g.ids.push_back(e.id);
    g.labels.push_back(e.name);
    int h = hub_idx[e.hub];
    g.hub_of.push_back(h);
    g.hub_size[h]++;
    bool is_coord = coord.count(e.hub) && coord.at(e.hub) == e.id;
    if (is_coord) g.hub_coord[h] = static_cast<int>(i);
    g.weight.push_back((e.cls == Cls::Essential ? 1.0 : 0.6) * (is_coord ? 1.1 : 1.0));
  }
  for (const auto& e : es)
    for (const auto& s : e.suppliers)
      if (!sup_idx.count(s)) {
        sup_idx[s] = g.n_entities + static_cast<int>(sup_idx.size());
        g.ids.push_back("sup:" + s);
        g.labels.push_back(s);
        g.hub_of.push_back(-1);
        g.weight.push_back(0.0);
      }
  g.adj.assign(g.ids.size(), {});
  auto recv = [&](int i) { return std::max(0.02, 1.0 - M[i]); };

  for (int i = 0; i < g.n_entities; ++i) {
    for (const auto& s : es[i].suppliers) {
      int si = sup_idx[s];
      g.adj[si].push_back({i, p.p_supplier_to_client * recv(i)});
      g.adj[i].push_back({si, p.p_client_to_supplier});
    }
    for (int j = 0; j < g.n_entities; ++j) {
      if (i == j || g.hub_of[i] != g.hub_of[j]) continue;
      double base = g.hub_coord[g.hub_of[i]] == i ? p.p_coordinator : p.p_hub_peer;
      g.adj[i].push_back({j, base * recv(j)});
    }
  }
  return g;
}

struct SimAcc {
  double impact = 0, affected = 0, hub_events = 0, hubs_disrupted = 0;
  std::vector<double> hits;
};

SimAcc simulate(const Graph& g, int source, int runs, uint64_t seed, double share) {
  SimAcc acc;
  acc.hits.assign(g.n_entities, 0.0);
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> U(0.0, 1.0);
  std::vector<int> mark(g.ids.size(), -1), stack, hub_hit(g.n_hubs);
  stack.reserve(g.ids.size());
  for (int r = 0; r < runs; ++r) {
    std::fill(hub_hit.begin(), hub_hit.end(), 0);
    std::vector<char> coord_hit(g.n_hubs, 0);
    stack.clear();
    stack.push_back(source);
    mark[source] = r;
    double impact = 0;
    int affected = 0;
    while (!stack.empty()) {
      int u = stack.back();
      stack.pop_back();
      if (u < g.n_entities) {
        impact += g.weight[u];
        ++affected;
        acc.hits[u] += 1;
        hub_hit[g.hub_of[u]]++;
        if (g.hub_coord[g.hub_of[u]] == u) coord_hit[g.hub_of[u]] = 1;
      }
      for (const Edge& e : g.adj[u])
        if (mark[e.to] != r && U(rng) < e.p) {
          mark[e.to] = r;
          stack.push_back(e.to);
        }
    }
    int disrupted = 0;
    for (int h = 0; h < g.n_hubs; ++h)
      if (coord_hit[h] || hub_hit[h] >= share * g.hub_size[h]) ++disrupted;
    acc.impact += impact;
    acc.affected += affected;
    acc.hubs_disrupted += disrupted;
    if (disrupted > 0) acc.hub_events += 1;
  }
  return acc;
}

uint64_t mix(uint64_t seed, const std::string& s) {
  uint64_t h = std::hash<std::string>{}(s);
  return seed ^ (h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

// Простий пул потоків: кожен потік бере наступний індекс з атомарного лічильника.
// Результат не залежить від кількості потоків, бо генератор випадкових чисел
// ініціалізується від ідентифікатора вузла, а не від номера потоку.
template <class F>
void parallel_for(int n, int threads, F f) {
  int t = threads > 0 ? threads : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  t = std::min(t, std::max(1, n));
  std::atomic<int> next{0};
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(t));
  for (int k = 0; k < t; ++k)
    pool.emplace_back([&] {
      for (int i; (i = next++) < n;) f(i);
    });
  for (auto& th : pool) th.join();
}

double systemic(const Graph& g, const CascadeParams& p, int runs) {
  int n = static_cast<int>(g.ids.size());
  std::vector<double> imp(n);
  parallel_for(n, p.threads, [&](int s) {
    imp[s] = simulate(g, s, runs, mix(p.seed, g.ids[s]), p.hub_disruption_share).impact / runs;
  });
  double sum = 0;
  for (double x : imp) sum += x;
  return n ? sum / n : 0;
}

}  // namespace

CascadeReport run_cascade(const std::vector<Entity>& es,
                          const std::vector<std::pair<std::string, std::string>>& hub_coordinators,
                          const CascadeParams& p, bool with_mitigation) {
  auto t0 = std::chrono::steady_clock::now();
  std::map<std::string, std::string> coord(hub_coordinators.begin(), hub_coordinators.end());
  Graph g = build(es, coord, p);
  int n = static_cast<int>(g.ids.size());
  CascadeReport rep;
  rep.seeds.resize(n);

  parallel_for(n, p.threads, [&](int s) {
    SimAcc a = simulate(g, s, p.runs, mix(p.seed, g.ids[s]), p.hub_disruption_share);
    SeedResult& r = rep.seeds[s];
    r.node = g.ids[s];
    r.label = g.labels[s];
    r.type = s < g.n_entities ? "entity" : "supplier";
    r.expected_impact = a.impact / p.runs;
    r.expected_affected = a.affected / p.runs;
    r.p_hub_disruption = a.hub_events / p.runs;
    r.expected_hubs_disrupted = a.hubs_disrupted / p.runs;
    std::set<int> hubs;
    std::vector<std::pair<std::string, double>> hit;
    for (int i = 0; i < g.n_entities; ++i) {
      double pr = a.hits[i] / p.runs;
      if (pr > 0.01) hubs.insert(g.hub_of[i]);
      if (i != s) hit.emplace_back(g.ids[i], pr);
    }
    r.hubs_reached = static_cast<int>(hubs.size());
    std::sort(hit.begin(), hit.end(), [](auto& x, auto& y) { return x.second > y.second; });
    if (hit.size() > 5) hit.resize(5);
    r.top_affected = hit;
  });
  rep.simulations = static_cast<long long>(n) * p.runs;
  double sum = 0;
  for (const auto& r : rep.seeds) sum += r.expected_impact;
  rep.systemic_risk = n ? sum / n : 0;
  std::sort(rep.seeds.begin(), rep.seeds.end(),
            [](const SeedResult& a, const SeedResult& b) { return a.expected_impact > b.expected_impact; });

  if (with_mitigation) {
    int mruns = std::max(1000, p.runs / 4);
    double base = systemic(g, p, mruns);
    rep.simulations += static_cast<long long>(n) * mruns;
    for (int k = 0; k < g.n_entities; ++k) {
      Graph gk = build(es, coord, p, k);
      double after = systemic(gk, p, mruns);
      rep.simulations += static_cast<long long>(n) * mruns;
      MitigationResult m;
      m.entity = es[k].id;
      m.name = es[k].name;
      m.risk_before = base;
      m.risk_after = after;
      m.reduction = base - after;
      m.reduction_pct = base > 0 ? 100.0 * (base - after) / base : 0;
      rep.mitigation.push_back(m);
    }
    std::sort(rep.mitigation.begin(), rep.mitigation.end(),
              [](const MitigationResult& a, const MitigationResult& b) { return a.reduction > b.reduction; });
  }
  rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return rep;
}

Value CascadeReport::to_json() const {
  json::Array s, m;
  for (const auto& r : seeds) {
    json::Array top;
    for (const auto& [id, pr] : r.top_affected) top.push_back(Value(json::Object{{"entity", id}, {"p", pr}}));
    s.push_back(Value(json::Object{{"node", r.node},
                                   {"label", r.label},
                                   {"type", r.type},
                                   {"expected_impact", r.expected_impact},
                                   {"expected_affected", r.expected_affected},
                                   {"p_hub_disruption", r.p_hub_disruption},
                                   {"expected_hubs_disrupted", r.expected_hubs_disrupted},
                                   {"hubs_reached", r.hubs_reached},
                                   {"top_affected", Value(std::move(top))}}));
  }
  for (const auto& x : mitigation)
    m.push_back(Value(json::Object{{"entity", x.entity},
                                   {"name", x.name},
                                   {"risk_before", x.risk_before},
                                   {"risk_after", x.risk_after},
                                   {"reduction", x.reduction},
                                   {"reduction_pct", x.reduction_pct}}));
  return Value(json::Object{{"systemic_risk", systemic_risk},
                            {"simulations", simulations},
                            {"seconds", seconds},
                            {"seeds", Value(std::move(s))},
                            {"mitigation", Value(std::move(m))}});
}

}  // namespace pao
