.PHONY: validate configure build test format-check

PYTHON ?= python3
CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build/developer

validate:
	$(PYTHON) tools/validate.py

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug

build: configure
	$(CMAKE) --build $(BUILD_DIR)

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

format-check: configure
	$(CMAKE) --build $(BUILD_DIR) --target format-check
