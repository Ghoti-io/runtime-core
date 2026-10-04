# Fixture for the gate self-test; not part of the library.
TEST_GATES ?= check-symbols check-aliasing check-stamps check-labels \
	check-direction check-edges check-gates check-version check-wiring check-hook

test: all $(TEST_GATES)
	@echo test

check-symbols:
ifeq ($(OS_NAME), Linux)
	@tools/check-symbols.sh $(APP_DIR)/$(TARGET) $(LIBVER_SYMBOL) .
else
	@echo skipped
endif

check-aliasing: ## text
	@$(CC) -Wstrict-aliasing=1 probe.c

check-stamps:
	@python3 tools/check-stamps.py

check-labels:
	@tools/check-labels.sh .

check-direction:
	@tools/check-direction.sh .

check-edges: $(APP_DIR)/$(TARGET)
	@tools/check-edges.sh --includes .
	@tools/check-edges.sh --links $(APP_DIR)

check-gates:
	@tools/check-gates.sh

check-version:
	@tools/check-version.sh Makefile .

check-wiring:
	@python3 tools/check-wiring.py

check-hook:
	@tools/check-hook.sh .githooks/commit-msg
