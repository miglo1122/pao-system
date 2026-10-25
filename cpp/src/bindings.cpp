// Python-модуль _paocore: пряме вбудовування ядра в процес Python без subprocess.
// Інтерфейс рядковий (JSON), щоб не дублювати типи в двох мовах.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "pao/cascade.hpp"
#include "pao/engine.hpp"

namespace py = pybind11;
using namespace pao;

class PyEngine {
 public:
  PyEngine(const std::string& registry_json, const std::string& config_json)
      : eng_(Config::from_json(config_json.empty() ? Value(json::Object{}) : json::parse(config_json))) {
    eng_.load_registry(json::parse(registry_json));
  }
  std::vector<std::string> process(const std::string& event_json) {
    std::vector<std::string> out;
    for (const auto& d : eng_.process(json::parse(event_json))) out.push_back(d.dump());
    return out;
  }
  std::vector<std::string> process_many(const std::vector<std::string>& events) {
    std::vector<std::string> out;
    py::gil_scoped_release release;
    for (const auto& e : events)
      for (const auto& d : eng_.process(json::parse(e))) out.push_back(d.dump());
    return out;
  }
  std::string snapshot() const { return eng_.snapshot().dump(); }
  std::string cascade(const std::string& params_json, bool mitigation) {
    CascadeParams p =
        CascadeParams::from_json(params_json.empty() ? Value(json::Object{}) : json::parse(params_json));
    CascadeReport r;
    {
      py::gil_scoped_release release;
      r = run_cascade(eng_.entities(), eng_.hub_coordinators(), p, mitigation);
    }
    return r.to_json().dump();
  }
  double now() const { return eng_.now(); }

 private:
  Engine eng_;
};

PYBIND11_MODULE(_paocore, m) {
  m.doc() = "Ядро моделі проактивного адміністративного нагляду (PAO), C++17";
  py::class_<PyEngine>(m, "Engine")
      .def(py::init<const std::string&, const std::string&>(), py::arg("registry_json"),
           py::arg("config_json") = "")
      .def("process", &PyEngine::process)
      .def("process_many", &PyEngine::process_many)
      .def("snapshot", &PyEngine::snapshot)
      .def("cascade", &PyEngine::cascade, py::arg("params_json") = "", py::arg("mitigation") = true)
      .def_property_readonly("now", &PyEngine::now);
  m.def("add_working_days", &add_working_days);
}
