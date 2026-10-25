# PAO — команди розробника. Довідка: make help
PY  ?= python3
CXX ?= c++
export PYTHONPATH := $(CURDIR)/python

SAN_FLAGS := -std=c++17 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror -Icpp/include -pthread
CORE      := cpp/src/engine.cpp cpp/src/cascade.cpp

.PHONY: help all deps build test demo serve cascade bench lint format sanitize coverage clean

help:  ## показати цю довідку
	@grep -E '^[a-z]+:.*##' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  make %-10s %s\n", $$1, $$2}'

all: build test  ## зібрати й протестувати

deps:  ## встановити бібліотеки Python
	$(PY) -m pip install numpy pybind11

build:  ## зібрати C++ ядро і Python-модуль
	@if command -v cmake >/dev/null; then cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j; else bash build.sh; fi

test: build  ## усі тести: C++, Python, звірка C++ ↔ Python
	./build/pao_tests
	$(PY) -m unittest discover -s python/tests -v
	$(PY) -m pao verify

demo: build  ## 30-денний сценарій → out/report.html
	$(PY) -m pao demo --out out

serve: build  ## веб-панель http://127.0.0.1:8080
	$(PY) -m pao serve --port 8080 --demo 12

cascade: build  ## рейтинг системних точок відмови
	$(PY) -m pao cascade --runs 50000

bench: build  ## продуктивність ядра
	./build/pao_engine bench --registry data/registry.json --events 500000

lint:  ## статичний аналіз (ruff, clang-format, clang-tidy)
	ruff check python
	ruff format --check python
	clang-format --dry-run --Werror cpp/include/pao/*.hpp cpp/src/*.cpp cpp/tests/*.cpp
	clang-tidy -quiet --checks='-*,bugprone-*,performance-*,-bugprone-easily-swappable-parameters,-bugprone-narrowing-conversions' \
	  --warnings-as-errors='*' $(CORE) cpp/src/main.cpp -- -std=c++17 -Icpp/include

format:  ## автоформатування коду
	ruff format python
	ruff check python --fix
	clang-format -i cpp/include/pao/*.hpp cpp/src/*.cpp cpp/tests/*.cpp

sanitize:  ## тести під AddressSanitizer, UBSan і ThreadSanitizer
	mkdir -p build/san
	$(CXX) $(SAN_FLAGS) -fsanitize=address,undefined $(CORE) cpp/tests/test_engine.cpp -o build/san/tests_asan
	./build/san/tests_asan
	$(CXX) $(SAN_FLAGS) -fsanitize=thread $(CORE) cpp/src/main.cpp -o build/san/engine_tsan
	./build/san/engine_tsan cascade --registry data/registry.json --runs 2000 --threads 4 >/dev/null

coverage:  ## покриття коду тестами (gcovr, coverage.py)
	mkdir -p build/cov
	$(CXX) -std=c++17 -O0 --coverage -Icpp/include -pthread $(CORE) cpp/tests/test_engine.cpp -o build/cov/tests
	cd build/cov && ./tests >/dev/null
	gcovr -r . --filter cpp/src --filter cpp/include build/cov
	coverage run --source=python/pao -m unittest discover -s python/tests -q
	coverage run -a --source=python/pao -m pao demo --out build/cov/out --replications 3 >/dev/null
	coverage run -a --source=python/pao -m pao verify >/dev/null
	coverage report

clean:  ## видалити згенероване
	rm -rf build out python/pao/_paocore*.so .coverage
