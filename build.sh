#!/usr/bin/env bash
# Збирання без CMake: лише компілятор C++ (clang на macOS, g++ на Linux).
set -e
cd "$(dirname "$0")"
CXX="${CXX:-c++}"
FLAGS="-std=c++17 -O2 -Wall -Icpp/include -pthread"
CORE="cpp/src/engine.cpp cpp/src/cascade.cpp"
mkdir -p build

$CXX $FLAGS $CORE cpp/src/main.cpp -o build/pao_engine
$CXX $FLAGS $CORE cpp/tests/test_engine.cpp -o build/pao_tests

if python3 -c "import pybind11" 2>/dev/null; then
  EXT=$(python3 -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")
  INC=$(python3 -m pybind11 --includes)
  LINK=""
  [ "$(uname)" = "Darwin" ] && LINK="-undefined dynamic_lookup"
  $CXX $FLAGS -fPIC -shared $INC $CORE cpp/src/bindings.cpp -o "python/pao/_paocore$EXT" $LINK
fi
