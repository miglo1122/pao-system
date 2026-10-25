#!/usr/bin/env bash
# Запуск PAO однією командою: перевірка інструментів → залежності → збирання → тести → демо → панель.
set -e
cd "$(dirname "$0")"
export PYTHONPATH="$PWD/python"
PORT="${PORT:-8080}"

say() { printf '\n\033[1;34m== %s\033[0m\n' "$1"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "Не знайдено: $1. $2"; exit 1; }; }

say "1/5 Перевірка інструментів"
need c++     "macOS: xcode-select --install; Ubuntu/WSL: sudo apt install -y g++ python3-pip"
need python3 "macOS: xcode-select --install; Ubuntu/WSL: sudo apt install -y python3"
echo "Компілятор: $(c++ --version | head -1)"
echo "$(python3 --version)"
command -v cmake >/dev/null 2>&1 && echo "$(cmake --version | head -1)" || echo "CMake не знайдено: збираю напряму компілятором"

say "2/5 Бібліотеки Python (numpy, pybind11)"
if ! python3 -c "import numpy" 2>/dev/null || ! python3 -c "import pybind11" 2>/dev/null; then
  python3 -m pip install --user numpy pybind11 2>/dev/null \
    || python3 -m pip install --user --break-system-packages numpy pybind11 \
    || { echo "Не вдалося встановити. Спробуйте: sudo apt install -y python3-numpy python3-pybind11"; exit 1; }
fi
python3 -c "import numpy; print('numpy', numpy.__version__)"

say "3/5 Збирання C++ ядра"
if command -v cmake >/dev/null 2>&1; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j >/dev/null
else
  bash build.sh
fi
if ! ls python/pao/_paocore*.so >/dev/null 2>&1; then
  echo "Не зібрано модуль _paocore для Python. Перевірте: python3 -m pip install --user pybind11"
  exit 1
fi
echo "Зібрано: build/pao_engine + модуль _paocore для Python"

say "4/5 Тести"
./build/pao_tests | tail -1
python3 python/tests/test_pao.py 2>&1 | tail -1
python3 -m pao verify

say "5/5 Демонстраційний запуск і звіт"
python3 -m pao demo --out out | tail -1
echo "Звіт: $PWD/out/report.html"
echo "Акти: $PWD/out/acts/"

say "Панель наглядача: відкрийте http://127.0.0.1:$PORT у браузері (зупинка: Ctrl+C)"
exec python3 -m pao serve --host 127.0.0.1 --port "$PORT" --demo 12
