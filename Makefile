SCRIPTS_DIR := buildroot/share/scripts
MAKESCRIPTS_DIR := buildroot/share/make
CONTAINER_RT_BIN := docker
CONTAINER_RT_OPTS := --rm -v $(PWD):/code -v platformio-cache:/root/.platformio
CONTAINER_IMAGE := marlin-dev
UNIT_TEST_CONFIG ?= default

# Find a Python 3 interpreter
ifeq ($(OS),Windows_NT)
	# Windows: use `where` – fall back through the three common names
	PYTHON := $(shell which python 2>nul || which python3 2>nul || which py 2>nul)
	# Windows: Use Python script to find pins files
	ALL_PINS := $(shell $(PYTHON) $(MAKESCRIPTS_DIR)/find.py Marlin/src/pins -mindepth 2 -name 'pins_*.h')
else
	# POSIX: use `command -v` – prefer python3 over python
	PYTHON := $(shell command -v python3 2>/dev/null || command -v python 2>/dev/null)
	# Unix/Linux: Use find command
	ALL_PINS := $(shell find Marlin/src/pins -mindepth 2 -name 'pins_*.h')
endif

PINSPATH ?=
PINSPATH := $(patsubst Marlin/src/pins/%,%,$(PINSPATH))
PINSPATH := $(patsubst %/,%,$(PINSPATH))

# If PINSPATH already contains %, use it directly.
# Otherwise append /% to match all files below the path.
PINS := $(if $(PINSPATH),$(filter Marlin/src/pins/$(PINSPATH)/%,$(ALL_PINS)),$(ALL_PINS))

# Check that the found interpreter is Python 3
# Error if there's no Python 3 available
ifneq ($(strip $(PYTHON)),)
	PYTHON_VERSION := $(shell $(PYTHON) -c "import sys; print(sys.version_info[0])" 2>/dev/null)
	ifneq ($(PYTHON_VERSION),3)
		$(error $(PYTHON) is not Python 3 – install a Python‑3.x interpreter or adjust your PATH)
	endif
else
	$(error No Python executable found – install Python 3.x and make sure it is in your PATH)
endif

help:
	@echo "Tasks for local development:"
	@echo "make marlin                    : Build Marlin for the configured board"
	@echo "make format-pins -j            : Reformat all pins files (-j for parallel execution)"
	@echo "make format-pins -j PINSPATH=dir : Reformat only pins files under dir"
	@echo "make validate-lines -j         : Validate line endings, fails on trailing whitespace, etc."
	@echo "make validate-pins -j          : Validate all pins files, fails if any require reformatting"
	@echo "make validate-boards -j        : Validate boards.h and pins.h for standards compliance"
	@echo "make validate-urls             : Validate URLs in source files"
	@echo "make tests-single-ci           : Run a single test from inside the CI"
	@echo "make tests-single-local        : Run a single test locally"
	@echo "make tests-single-local-docker : Run a single test locally, using docker"
	@echo "make tests-all-local           : Run all tests locally"
	@echo "make tests-all-local-docker    : Run all tests locally, using docker"
	@echo "make unit-test-single-local    : Run unit tests for a single config locally"
	@echo "make unit-test-single-local-docker : Run unit tests for a single config locally, using docker"
	@echo "make unit-test-all-local       : Run all code tests locally (test HAL)"
	@echo "make unit-test-coverage        : Run one config's unit tests with gcov, report coverage"
	@echo "make unit-test-mutation        : Mutation-test one source file (TARGET=path/to/file.cpp)"
	@echo "make unit-test-asan            : Run one config's unit tests under AddressSanitizer"
	@echo "make unit-test-all-local-docker : Run all code tests locally, using docker"
	@echo "make setup-local-docker        : Setup local docker"
	@echo ""
	@echo "Options for testing:"
	@echo "  TEST_TARGET          Set when running tests-single-*, to select the"
	@echo "                       test. If you set it to ALL it will run all "
	@echo "                       tests, but some of them are broken: use "
	@echo "                       tests-all-* instead to run only the ones that "
	@echo "                       run on GitHub CI"
	@echo "  ONLY_TEST            Limit tests to only those that contain this, or"
	@echo "                       the index of the test (1-based)"
	@echo "  UNIT_TEST_CONFIG     Set the name of the config from the test folder, without"
	@echo "                       the leading number. Default is 'default'". Used with the
	@echo "                       unit-test-single-* tasks"
	@echo "  UNIT_TEST_ENV        Environment the unit tests run in. Defaults to"
	@echo "                       testhal_native_test, the only one where time can be"
	@echo "                       advanced on request so motion and the ISRs are reachable"
	@echo "  COVERAGE_ENV         Suite to measure coverage for. Must name the same suite"
	@echo "                       as MUTATION_ENV, or mutants are restricted to lines a"
	@echo "                       different suite covered"
	@echo "  MUTATION_ENV         Suite to mutation-test against. Carry it on RERUN= too"
	@echo "  MUTATION_MUTANT_DIR  Where generated mutants go — several GB per target, kept"
	@echo "                       so RERUN= can read them back"
	@echo "  VERBOSE_PLATFORMIO   If you want the full PIO output, set any value"
	@echo "  GIT_RESET_HARD       Used by CI: reset all local changes. WARNING:"
	@echo "                       THIS WILL UNDO ANY CHANGES YOU'VE MADE!"

marlin:
	./buildroot/bin/mftest -a
.PHONY: marlin

clean:
	rm -rf .pio/build*

tests-single-ci:
	export GIT_RESET_HARD=true
	$(MAKE) tests-single-local TEST_TARGET=$(TEST_TARGET) PLATFORMIO_BUILD_FLAGS=-DGITHUB_ACTION

tests-single-local:
	@if ! test -n "$(TEST_TARGET)" ; then echo "***ERROR*** Set TEST_TARGET=<your-module> or use make tests-all-local" ; return 1; fi
	export PATH="./buildroot/bin/:./buildroot/tests/:${PATH}" \
	  && export VERBOSE_PLATFORMIO=$(VERBOSE_PLATFORMIO) \
	  && run_tests . $(TEST_TARGET) "$(ONLY_TEST)"

tests-single-local-docker:
	@if ! test -n "$(TEST_TARGET)" ; then echo "***ERROR*** Set TEST_TARGET=<your-module> or use make tests-all-local-docker" ; return 1; fi
	@if ! $(CONTAINER_RT_BIN) images -q $(CONTAINER_IMAGE) > /dev/null ; then $(MAKE) setup-local-docker ; fi
	$(CONTAINER_RT_BIN) run $(CONTAINER_RT_OPTS) $(CONTAINER_IMAGE) make tests-single-local TEST_TARGET=$(TEST_TARGET) VERBOSE_PLATFORMIO=$(VERBOSE_PLATFORMIO) GIT_RESET_HARD=$(GIT_RESET_HARD) ONLY_TEST="$(ONLY_TEST)"

tests-all-local:
	@$(PYTHON) -c "import yaml" 2>/dev/null || (echo 'pyyaml module is not installed. Install it with "$(PYTHON) -m pip install pyyaml"' && exit 1)
	export PATH="./buildroot/bin/:./buildroot/tests/:${PATH}" \
	  && export VERBOSE_PLATFORMIO=$(VERBOSE_PLATFORMIO) \
	  && for TEST_TARGET in $$($(PYTHON) $(MAKESCRIPTS_DIR)/get_test_targets.py) ; do \
	    if [ "$$TEST_TARGET" = "linux_native" ] && [ "$$(uname)" = "Darwin" ]; then \
	      echo "Skipping tests for $$TEST_TARGET on macOS" ; \
	      continue ; \
	    fi ; \
	    echo "Running tests for $$TEST_TARGET" ; \
	    run_tests . $$TEST_TARGET || exit 1 ; \
	    sleep 5; \
	  done

tests-all-local-docker:
	@if ! $(CONTAINER_RT_BIN) images -q $(CONTAINER_IMAGE) > /dev/null ; then $(MAKE) setup-local-docker ; fi
	$(CONTAINER_RT_BIN) run $(CONTAINER_RT_OPTS) $(CONTAINER_IMAGE) make tests-all-local VERBOSE_PLATFORMIO=$(VERBOSE_PLATFORMIO) GIT_RESET_HARD=$(GIT_RESET_HARD)

# The suite, and the only one. Unit tests build against HAL/TEST, where time advances only
# when a test asks — so motion, blocking commands and the interrupt handlers are reachable,
# and a run is a function of the code rather than of how busy the machine was.
#
# There is deliberately no LINUX-HAL equivalent any more. That HAL backs its peripherals on
# real OS facilities, which is right for a board and wrong for a test, and the arrangement
# failed in both directions: every command that waits was unreachable there, and the tests
# that could run were slow enough that nobody ran them — the build was broken for seven days
# and 56 commits before anyone noticed. HAL/LINUX is still built, as firmware, by
# `env:linux_native`.
UNIT_TEST_ENV ?= testhal_native_test

unit-test-single-local:
	platformio run -t marlin_$(UNIT_TEST_CONFIG) -e $(UNIT_TEST_ENV)

unit-test-single-local-docker:
	@if ! $(CONTAINER_RT_BIN) images -q $(CONTAINER_IMAGE) > /dev/null ; then $(MAKE) setup-local-docker ; fi
	$(CONTAINER_RT_BIN) run $(CONTAINER_RT_OPTS)  $(CONTAINER_IMAGE) make unit-test-single-local UNIT_TEST_CONFIG=$(UNIT_TEST_CONFIG)

unit-test-all-local:
	platformio run -t test-marlin -e $(UNIT_TEST_ENV)

# The same tests against the LINUX HAL, where the peripherals are real OS facilities.
# Slower and historically the noisier of the two, so it is run deliberately rather than
# on every change — but it is the only thing that exercises that HAL, and it has earned
# its keep by failing when the test HAL could not.
COVERAGE_DIR ?= .pio/coverage

# Which suite to measure. Mutants are restricted to the lines this build marks covered,
# so COVERAGE_ENV must name the same suite as MUTATION_ENV below or the restriction is
# taken from the wrong measurement — silently, since both produce a plausible report.
#   make unit-test-coverage COVERAGE_ENV=testhal_native_coverage
COVERAGE_ENV ?= testhal_native_coverage

# Sources a test can never execute, excluded so that a coverage figure means "of the code a
# test could run" rather than being diluted by code that is not reachable by construction.
#
# `MarlinBoot.cpp` holds `setup()` and `loop()` — the two entry points the platform calls. A
# test build stands in for both: `SimulatedHardware::ensure_ready()` does the bring-up and each
# test drives `idle()` and `queue.advance()` itself. Together they were a third of
# `MarlinCore.cpp`, which reported 36% with no way to tell the untestable part from the untested
# part.
#
# Excluding a file is a claim that nothing in it can be tested, so the list is deliberately
# short and the file it names is meant to shrink: anything in there that can be named and
# called belongs back in a testable translation unit. Adding to this list should be an argument,
# not a convenience.
COVERAGE_EXCLUDES ?= --exclude 'Marlin/src/MarlinBoot.cpp'

# gcov's counters are not atomic, and several tests here drive the simulated pins from a second
# thread — the `SerialCapture` drainer, the kill button, the answer to a blocking `M0`. They all
# go through `WRITE`, which inlines `Gpio::valid_pin()`, so two threads increment the same branch
# counter and one of them underflows. gcovr treats a negative hit count as a parse error and
# **aborts the whole report**, losing the run.
#
# This is GCC PR68080 and it is a permanent property of measuring a threaded suite, not an
# intermittent fault: the report is abandoned over a branch counter on one line of a HAL header,
# while every line figure in it is unaffected. `warn_once_per_file` keeps the diagnostic — it
# names the file, so a new instance is still visible — without discarding the measurement.
#
# It is scoped to this one failure mode deliberately. Any other parse error should still be
# fatal, because any other parse error means the coverage data itself is not to be trusted.
COVERAGE_PARSE_ERRORS ?= --gcov-ignore-parse-errors=negative_hits.warn_once_per_file

unit-test-coverage:
	@command -v gcovr >/dev/null || (echo 'gcovr is not installed. Install it with "uv tool install gcovr" or "pipx install gcovr"' && exit 1)
	rm -rf .pio/build/$(COVERAGE_ENV) $(COVERAGE_DIR)
	platformio run -t marlin_$(UNIT_TEST_CONFIG) -e $(COVERAGE_ENV)
	@mkdir -p $(COVERAGE_DIR)/html
	gcovr -r . .pio/build/$(COVERAGE_ENV) \
	  --filter 'Marlin/src/' --exclude 'Marlin/tests/' $(COVERAGE_EXCLUDES) $(COVERAGE_PARSE_ERRORS) \
	  --txt $(COVERAGE_DIR)/summary.txt --print-summary \
	  --html-details $(COVERAGE_DIR)/html/index.html
	@echo ""
	@echo "--- Platform-agnostic (excludes Marlin/src/HAL/) — the figure quoted in docs/ ---"
	@gcovr -r . .pio/build/$(COVERAGE_ENV) \
	  --filter 'Marlin/src/' --exclude 'Marlin/tests/' --exclude 'Marlin/src/HAL/' $(COVERAGE_EXCLUDES) $(COVERAGE_PARSE_ERRORS) \
	  --txt $(COVERAGE_DIR)/summary-platform-agnostic.txt --print-summary
	@echo ""
	@echo "Measured: $(COVERAGE_ENV) / config $(UNIT_TEST_CONFIG)"
	@echo "HTML report: $(COVERAGE_DIR)/html/index.html"

# Mutation testing. Coverage says a line ran; mutation says a test noticed.
#   make unit-test-mutation TARGET=Marlin/src/gcode/parser.cpp
#   make unit-test-mutation TARGET=... MUTATION_ENV=acceptance_native_test
#   make unit-test-mutation TARGET=... RERUN=.pio/mutation/results.json   # survivors only
# Restricting mutants to covered lines needs a coverage build of the same suite; run
# "make unit-test-coverage" first with a matching COVERAGE_ENV, or pass
# MUTATION_COVERAGE= to mutate every line.
MUTATION_ENV ?= testhal_native_test
MUTATION_RESULTS ?= .pio/mutation/results.json

# Where the generated mutants live. One full copy of the target per mutant — a few GB for
# a large source file — kept after the run so RERUN= can read them back. Point this at a
# roomier filesystem when the repo's own is tight; a run per worktree multiplies it.
#   make unit-test-mutation TARGET=... MUTATION_MUTANT_DIR=/mnt/big/marlin-mutants
# A RERUN must name the same directory as the full run that produced its results file.
MUTATION_MUTANT_DIR ?=

# The suite under AddressSanitizer. Not part of the normal loop: it is slower, and a
# sanitizer abort stops the run at the first fault rather than reporting all of them, so
# it is a fix-and-repeat cycle rather than a measurement.
ASAN_ENV ?= testhal_native_asan

.PHONY: unit-test-asan
unit-test-asan:
	platformio run -t marlin_$(UNIT_TEST_CONFIG) -e $(ASAN_ENV)

unit-test-mutation:
	@if ! test -n "$(TARGET)" ; then echo "***ERROR*** Set TARGET=<source-file>" ; exit 1 ; fi
	@command -v mutate >/dev/null || (echo 'universalmutator is not installed. Install it with "uv tool install universalmutator"' && exit 1)
	MUTATION_MUTANT_DIR=$(MUTATION_MUTANT_DIR) \
	$(PYTHON) buildroot/share/scripts/mutation_test.py $(TARGET) \
	  --env $(MUTATION_ENV) --suite $(UNIT_TEST_CONFIG) --results $(MUTATION_RESULTS) \
	  $(if $(RERUN),--rerun-survivors $(RERUN),) \
	  $(if $(MUTATION_JOBS),--jobs $(MUTATION_JOBS),)

unit-test-all-local-docker:
	@if ! $(CONTAINER_RT_BIN) images -q $(CONTAINER_IMAGE) > /dev/null ; then $(MAKE) setup-local-docker ; fi
	$(CONTAINER_RT_BIN) run $(CONTAINER_RT_OPTS)  $(CONTAINER_IMAGE) make unit-test-all-local

USERNAME := $(shell whoami)
USER_ID  := $(shell id -u)
GROUP_ID := $(shell id -g)

.PHONY: setup-local-docker setup-local-docker-old

setup-local-docker:
	@echo "Building marlin-dev Docker image..."
	$(CONTAINER_RT_BIN) build -t $(CONTAINER_IMAGE) \
	  --build-arg USERNAME=$(USERNAME) \
	  --build-arg USER_ID=$(USER_ID) \
	  --build-arg GROUP_ID=$(GROUP_ID) \
	  -f docker/Dockerfile .
	@echo
	@echo "To run all tests in Docker:"
	@echo "  make tests-all-local-docker"
	@echo "To run a single test in Docker:"
	@echo "  make tests-single-local-docker TEST_TARGET=mega2560"

setup-local-docker-old:
	$(CONTAINER_RT_BIN) buildx build -t $(CONTAINER_IMAGE) -f docker/Dockerfile .

.PHONY: $(PINS) format-pins validate-pins

$(PINS): %:
	@echo "Formatting pins $@"
	@$(PYTHON) $(SCRIPTS_DIR)/pinsformat.py $< $@

format-pins: $(PINS)
	@echo "Processed $(words $(PINS)) pins files"

validate-pins: format-pins
	@echo "Validating pins files"
	@git diff --exit-code || (git status && echo "\nError: Pins files are not formatted correctly. Run \"make format-pins\" to fix.\n" && exit 1)

.PHONY: format-lines validate-lines validate-urls

format-lines:
	@echo "Formatting all sources"
	@$(PYTHON) $(SCRIPTS_DIR)/linesformat.py buildroot
	@$(PYTHON) $(SCRIPTS_DIR)/linesformat.py Marlin

validate-lines:
	@echo "Validating text formatting"
	@npx prettier --check . --editorconfig --object-wrap preserve --prose-wrap never

validate-urls:
	@echo "Checking URLs in source files"
	@$(MAKESCRIPTS_DIR)/check-urls.sh

BOARDS_FILE := Marlin/src/core/boards.h

.PHONY: validate-boards

validate-boards:
	@echo "Validating boards.h file"
	@$(PYTHON) $(MAKESCRIPTS_DIR)/validate_boards.py $(BOARDS_FILE) || (echo "\nError: boards.h file is not valid. Please check and correct it.\n" && exit 1)
