// pao_engine — консольний інтерфейс ядра PAO.
//
// Команди:
//   stream   потік подій JSON Lines зі stdin → рішення у stdout
//   assess   поточні оцінки всіх суб'єктів реєстру
//   cascade  моделювання каскадних атак і пріоритет аудитів
//   bench    вимірювання пропускної здатності ядра
//
// Приклад:
//   pao_engine stream --registry data/registry.json < events.jsonl > decisions.jsonl

#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>

#include "pao/cascade.hpp"
#include "pao/engine.hpp"

using namespace pao;

namespace {

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("не вдалося відкрити файл: " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Args {
  std::string cmd, registry, config, snapshot;
  int runs = 20000, threads = 0, events = 200000;
  long long seed = 20260324;
  bool mitigation = true;
};

Args parse_args(int argc, char** argv) {
  Args a;
  if (argc < 2) throw std::runtime_error("вкажіть команду: stream | assess | cascade | bench");
  a.cmd = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("бракує значення для " + k);
      return argv[++i];
    };
    if (k == "--registry")
      a.registry = next();
    else if (k == "--config")
      a.config = next();
    else if (k == "--snapshot")
      a.snapshot = next();
    else if (k == "--runs")
      a.runs = std::stoi(next());
    else if (k == "--threads")
      a.threads = std::stoi(next());
    else if (k == "--seed")
      a.seed = std::stoll(next());
    else if (k == "--events")
      a.events = std::stoi(next());
    else if (k == "--no-mitigation")
      a.mitigation = false;
    else
      throw std::runtime_error("невідомий параметр: " + k);
  }
  if (a.registry.empty()) throw std::runtime_error("потрібен --registry");
  return a;
}

Engine make_engine(const Args& a) {
  Value cv = a.config.empty() ? Value(json::Object{}) : json::parse(read_file(a.config));
  Engine e(Config::from_json(cv));  // from_json нормує ваги до суми 1
  e.load_registry(json::parse(read_file(a.registry)));
  return e;
}

int cmd_stream(const Args& a) {
  Engine eng = make_engine(a);
  std::ios::sync_with_stdio(false);
  for (const auto& d : eng.process(Value(json::Object{{"type", "tick"}, {"t", eng.now()}})))
    std::cout << d.dump() << '\n';
  std::string line;
  size_t lineno = 0;
  while (std::getline(std::cin, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#') continue;
    Value ev;
    try {
      ev = json::parse(line);
    } catch (const std::exception& ex) {
      std::cout << Value(json::Object{{"type", "parse_error"},
                                      {"line", static_cast<long long>(lineno)},
                                      {"reason", ex.what()}})
                       .dump()
                << '\n';
      continue;
    }
    for (const auto& d : eng.process(ev)) std::cout << d.dump() << '\n';
  }
  std::cout.flush();
  if (!a.snapshot.empty()) {
    std::ofstream f(a.snapshot);
    f << eng.snapshot().dump();
  }
  std::cerr << "PAO: оброблено подій " << eng.events_processed() << "\n";
  return 0;
}

int cmd_assess(const Args& a) {
  Engine eng = make_engine(a);
  eng.process(Value(json::Object{{"type", "tick"}, {"t", eng.now()}}));
  std::cout << eng.snapshot().dump() << '\n';
  return 0;
}

int cmd_cascade(const Args& a) {
  Engine eng = make_engine(a);
  CascadeParams p;
  p.runs = a.runs;
  p.seed = static_cast<uint64_t>(a.seed);
  p.threads = a.threads;
  CascadeReport r = run_cascade(eng.entities(), eng.hub_coordinators(), p, a.mitigation);
  std::cout << r.to_json().dump() << '\n';
  std::cerr << "PAO: " << r.simulations << " симуляцій за " << r.seconds << " с\n";
  return 0;
}

int cmd_bench(const Args& a) {
  Engine eng = make_engine(a);
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> U(0, 1);
  const auto& es = eng.entities();
  if (es.empty()) throw std::runtime_error("порожній реєстр");
  double t = eng.now();
  size_t decisions = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < a.events; ++i) {
    t += 60;
    const Entity& e = es[rng() % es.size()];
    Value ev(json::Object{{"type", "telemetry"}, {"t", t}, {"entity", e.id}, {"E", U(rng)}, {"M", U(rng)}});
    decisions += eng.process(ev).size();
  }
  double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << Value(json::Object{{"events", a.events},
                                  {"seconds", sec},
                                  {"events_per_second", a.events / sec},
                                  {"decisions", static_cast<long long>(decisions)},
                                  {"entities", static_cast<long long>(es.size())}})
                   .dump()
            << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Args a = parse_args(argc, argv);
    if (a.cmd == "stream") return cmd_stream(a);
    if (a.cmd == "assess") return cmd_assess(a);
    if (a.cmd == "cascade") return cmd_cascade(a);
    if (a.cmd == "bench") return cmd_bench(a);
    throw std::runtime_error("невідома команда: " + a.cmd);
  } catch (const std::exception& ex) {
    std::cerr << "pao_engine: " << ex.what() << "\n";
    return 2;
  }
}
