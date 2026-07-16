.PHONY: validate clean

PYTHON ?= python3

validate:
	$(PYTHON) tools/validate_pd0.py

clean:
	@find . -type d -name __pycache__ -prune -exec rm -rf {} +

