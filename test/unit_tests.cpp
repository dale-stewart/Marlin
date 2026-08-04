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
#include "src/module/planner.h"
#include <stdio.h>
#include <string>

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
#endif // HAS_MEDIA

#ifdef __PLAT_TEST__

  #include "src/HAL/TEST/hardware/Gpio.h"

  // What was attached before any test ran — the baseline a test is expected to restore.
  static Peripheral *baseline_peripherals[Gpio::pin_count + 1] = { nullptr };

  static void record_attached_peripherals() {
    for (int i = 0; i <= Gpio::pin_count; ++i) baseline_peripherals[i] = Gpio::pin_map[i].cb;
  }

  /**
   * Findings are collected rather than asserted on the spot.
   *
   * This runs after `UnityDefaultTestRun()` has returned, which is outside any test: Unity's
   * failure macros `longjmp` to a buffer that is no longer current, so failing here would
   * corrupt the run rather than report it. The leaks are recorded, the pin table is put
   * back, and one synthetic test at the end of the suite reports the lot.
   */
  static std::string peripheral_leaks;

  static void check_no_peripheral_was_left_attached(const std::string &test_name) {
    for (int i = 0; i <= Gpio::pin_count; ++i) {
      if (Gpio::pin_map[i].cb == baseline_peripherals[i]) continue;
      const bool left_attached = Gpio::pin_map[i].cb != nullptr;
      char line[160];
      snprintf(line, sizeof(line), "\n  %s %s pin %d",
        test_name.c_str(),
        left_attached ? "left a peripheral attached to" : "detached the peripheral on",
        i);
      peripheral_leaks += line;
      // Put it back, so one fault is reported once rather than by every test after it.
      Gpio::pin_map[i].cb = baseline_peripherals[i];
    }
  }

  /**
   * A peripheral registered with `Gpio::attachPeripheral()` is almost always a local of the
   * test that made it, so the registration has to be withdrawn when the object goes. When it
   * is not, the pointer stays in the pin table and the next write to that pin — from an
   * interrupt, in an unrelated test — calls a method on a returned stack frame. The damage
   * lands wherever that memory was reused, so the test that fails is never the test at
   * fault, and it fails by corruption rather than by assertion. Register #26 was this.
   *
   * Detaching one that was there before the run is the same fault seen from the other side:
   * the peripheral is still alive but no longer being told about its pin.
   */
  MARLIN_TEST(harness, no_test_left_the_pin_table_disturbed) {
    if (!peripheral_leaks.empty())
      TEST_FAIL_MESSAGE(("peripherals were left registered to dead objects:" + peripheral_leaks).c_str());
  }

#else
  static void record_attached_peripherals() {}
  static void check_no_peripheral_was_left_attached(const std::string&) {}
#endif

// Install the stand-ins a test cannot opt out of, once, before the first test runs.
static void prepare_simulated_peripherals() {
  record_attached_peripherals();
  #if HAS_MEDIA
    card.changeMedia(&simulated_card());
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
static std::string current_test_name;

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

  // A card told to refuse writes stays that way until something says otherwise, and a
  // test that fails while injecting the fault never reaches its own cleanup. Clearing it
  // here rather than in a scope guard is the same reasoning as the heater targets above.
  #if HAS_MEDIA
    simulated_card().allow_writes();
  #endif

  /**
   * Leave nothing planned, either.
   *
   * A block in the planner is motion the machine still intends to perform, and the thing that
   * would consume it — the stepper interrupt, driven from the main loop — is not running
   * between tests. So a test that queues a move and does not run it hands the next test a
   * machine that is busy. That is not merely untidy: `planner.synchronize()` spins on
   * `idle()` until the queue drains, and `idle()` in a test build eventually reaches `kill()`
   * unless the kill button has been released for the duration — so the symptom is a *later*
   * test hanging, in some configurations and not others, with nothing wrong with it.
   *
   * `clear_block_buffer()` rather than `quick_stop()`: the latter sets a counter that only
   * the temperature interrupt clears, which would leave the machine busy for a different
   * reason.
   */
  planner.clear_block_buffer();

  /**
   * Leave no peripheral pointing at a dead object.
   *
   * A simulated peripheral is registered with `Gpio::attachPeripheral()` and is almost
   * always a local of the test that made it, so the registration has to be withdrawn when
   * the object goes. When it is not, the pointer stays in the pin table and the *next*
   * write to that pin — from an interrupt, in an unrelated test — calls a method on a
   * returned stack frame. The damage lands wherever that memory has been reused, so the
   * test that fails is never the test at fault, and it fails by corruption rather than by
   * assertion. Register #26 was exactly this, and it cost a full diagnosis to find.
   *
   * Checking here turns it into a named failure at the test that caused it. The comparison
   * is against what was attached before the run started rather than against nothing,
   * because a fixture may legitimately install something for the whole process.
   */
  check_no_peripheral_was_left_attached(current_test_name);
}

void MarlinTest::run() {
  Unity.TestFile = file.c_str();
  current_test_name = name;
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
