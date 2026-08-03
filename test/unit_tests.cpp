/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2024 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * unit_tests.cpp - Unit for running tests in the Marlin/tests/ folder.
 *
 * Provide the main() function used for all compiled unit test binaries.
 * It collects all the tests defined in the code and runs them through Unity.
 */

#include "unit_tests.h"
#include "src/module/temperature.h"

/**
 * The registry, constructed on first use rather than at static-initialisation time.
 *
 * Every MARLIN_TEST expands to a namespace-scope object in some other translation unit,
 * and each of those registers itself from its own constructor — during static
 * initialisation, in an order the standard leaves to the linker. A plain
 * `static std::list` here is initialised in *this* file's turn, so any test whose
 * translation unit is initialised first would push into a list that has not been
 * constructed yet. That is undefined behaviour, and it does not fail cleanly: it either
 * corrupts the heap outright or survives until the real construction resets the list's
 * head and silently discards every registration made before it.
 *
 * A function-local static is constructed on the first call instead — which is necessarily
 * the first registration — so the order the linker chooses stops mattering.
 */
static std::list<MarlinTest*>& all_marlin_tests() {
  static std::list<MarlinTest*> tests;
  return tests;
}

MarlinTest::MarlinTest(const std::string& _name, const void(*_test)(), const char *_file, const int _line)
: name(_name), test(_test), file(_file), line(_line) {
  all_marlin_tests().push_back(this);
}

#if HAS_MEDIA

  #include "src/sd/cardreader.h"
  #include "tests/support/simulated_media.h"

  /**
   * A formatted card, present from the first test.
   *
   * `Marlin::idle()` calls `CardReader::manage_media()`, so *every* test that waits for
   * anything reaches the media layer whether it cares about media or not. Without a
   * stand-in that ends in `Sd2Card::init()`, which polls a card over SPI and gives up on
   * a deadline: `while (cardCommand(CMD0, 0) != R1_IDLE_STATE) if (ELAPSED(millis(), …))`.
   * Both halves fail here — nothing is on the simulated bus so no card ever answers, and
   * under a HAL where time only advances when a test asks, `millis()` does not move
   * inside the loop, so the deadline never arrives. Production code that is correct on
   * hardware becomes an infinite loop.
   *
   * An empty slot is not enough either. Reporting "no card" leaves `CardReader::root`
   * unmounted, and the firmware then walks into defect #24 — `jobRecoverFileExists()`
   * opens a file on an unmounted volume without checking, and dereferences null. A
   * mounted volume is both the more useful default and the one that keeps the suite
   * running; #24 stays recorded and is pinned by its own test rather than by crashing
   * every configuration that enables media.
   */
  static SimulatedMedia simulated_card;

#endif // HAS_MEDIA

// Install the stand-ins a test cannot opt out of, once, before the first test runs.
static void prepare_simulated_peripherals() {
  #if HAS_MEDIA
    card.changeMedia(&simulated_card);
    card.mount();
  #endif
}

/**
 * Put the simulated peripherals back to rest between tests.
 *
 * Tests share one process, so a peripheral left running by one test is still running
 * during the next. Under the native HAL that is not merely untidy: its timers are POSIX
 * timers delivering real signals, and one left armed at a stepper-interrupt rate
 * interrupts every blocking call in the process from then on. `sleep_for()` restarts on
 * EINTR, so a two-millisecond delay in a later test stops finishing at all — which
 * presents as an unrelated test hanging, and only in the orders that happen to run the
 * offending test first.
 *
 * Disarming after every test makes the suite's result independent of the order it runs
 * in, which is the property that lets a mutation harness link the objects in whatever
 * order it finds them.
 */
static void quiesce_simulated_peripherals() {
  #ifdef __PLAT_LINUX__
    HAL_timer_stop_all();
  #endif

  /**
   * Leave nothing hot behind.
   *
   * A failing assertion does not return — Unity's failure path is a `longjmp` back into
   * `UnityDefaultTestRun`, which unwinds no C++ stack, so a test's own cleanup is skipped
   * and so is every destructor it was relying on. A heater target set for a test that
   * then fails therefore survives into the next one, and the first later test that waits
   * on temperature never finishes.
   *
   * That matters beyond tidiness, because it corrupts the measurement. Under mutation a
   * detected mutant should be *killed*; if the failing assertion also leaves a target
   * set, the run instead hangs somewhere later and scores TIMEOUT. The mutant is still
   * detected, but the number that says how — the killed-by-assertion count — is wrong,
   * and it is the only number worth reading. Called from `MarlinTest::run()` after the
   * test returns, which is on the far side of the `longjmp` and so runs either way.
   */
  #if HAS_HOTEND
    HOTEND_LOOP() thermalManager.setTargetHotend(0, e);
  #endif
  TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(0));
  TERN_(HAS_HEATED_CHAMBER, thermalManager.setTargetChamber(0));
}

void MarlinTest::run() {
  Unity.TestFile = file.c_str();
  UnityDefaultTestRun((UnityTestFunction)test, name.c_str(), line);
  quiesce_simulated_peripherals();
}

void run_all_marlin_tests() {
  prepare_simulated_peripherals();
  for (const auto registration : all_marlin_tests()) {
    registration->run();
  }
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  run_all_marlin_tests();
  UNITY_END();
  return 0;
}
