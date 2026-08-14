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
#include "src/module/motion.h"
#include "src/module/printcounter.h"
#include "src/gcode/queue.h"
#include "src/feature/pause.h"
#if ENABLED(EMERGENCY_PARSER)
  #include "src/feature/e_parser.h"
#endif
#if HAS_FILAMENT_SENSOR
  #include "src/feature/runout.h"
#endif
#include "tests/support/simulated_hardware.h"
#include "tests/gcode/serial_capture.h"
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

// Install the stand-ins a test cannot opt out of, once, before the first test runs.
/**
 * The scale the machine settled at — see quiesce_simulated_peripherals().
 *
 * Not recorded before the first test, which is the obvious place and the wrong one:
 * `Planner::settings` is a zero-initialised static until something calls `settings.reset()`,
 * and no fixture has run yet. Recording there captures all zeros, and restoring to zero
 * after every test gives every axis an infinite millimetres-per-step — which then hangs the
 * suite exactly as the leftover it was meant to prevent. Recorded instead on the first
 * boundary where the values are real, which is after the first test that sets the machine up.
 */
static float settled_steps_per_mm[DISTINCT_AXES];
static bool steps_per_mm_recorded = false;

static void prepare_simulated_peripherals() {
  record_attached_peripherals();
  SimulatedHardware::release_panel_buttons();
  #if HAS_MEDIA
    card.changeMedia(&simulated_card());
    card.mount();
  #endif
}

/**
 * Put the simulated peripherals back to rest between tests.
 *
 * Tests share one process, so anything one test leaves running is still running during the
 * next, and the result stops being a function of the code and starts being a function of
 * the order. Everything below exists because some version of that cost a diagnosis.
 *
 * This used to begin by disarming POSIX timers, because the suite could also be built
 * against HAL/LINUX, where a timer left armed at a stepper-interrupt rate interrupted every
 * blocking call in the process from then on — register #20. That build is gone: unit tests
 * run against HAL/TEST only, where a timer is state rather than a signal and cannot reach
 * out of the test that armed it. The rest of this function still matters, because state left
 * in the *firmware* travels between tests on any HAL.
 */
static std::string current_test_name;

static void quiesce_simulated_peripherals() {
  // A test that fails part-way through a click leaves the button held for every test after
  // it — same reasoning as the heater targets below.
  SimulatedHardware::release_panel_buttons();

  /**
   * Leave no port claiming a host that is not there.
   *
   * `SerialCapture` marks a port as connected and drains it from a second thread, and puts
   * both back in its destructor — which the `longjmp` described below does not run. What is
   * left is the worst possible combination: a port that busy-waits for room in a 128-byte
   * buffer, and no longer anything emptying it. The next write of more than 128 bytes never
   * returns, from whichever unrelated test happens to make it.
   *
   * That is how a *failed* capture turns into a *hung suite*, and the hang is nowhere near
   * the test at fault. Marking the ports unattached here is the same state they are given at
   * power-on, and it costs nothing when the destructor did run.
   */
  SerialCapture::release_live_captures();

  MYSERIAL1.host_connected = false;
  #ifdef LCD_SERIAL
    LCD_SERIAL.host_connected = false;
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

  /**
   * ...and nothing printing, which is the same leak one step downstream.
   *
   * Setting a hot target *starts the print job timer* — that is the firmware's rule, not
   * the harness's — so any test that heats the nozzle and does not explicitly stop the
   * job leaves the machine claiming to be printing for every test after it. Zeroing the
   * target above does not stop the timer.
   *
   * That is not cosmetic on a machine with a filament sensor. `should_monitor_runout()`
   * is `did_pause_print || printingIsActive()`, so a leaked job arms the sensor, and the
   * first later test that lets the machine idle gets `FILAMENT_RUNOUT_SCRIPT` injected
   * into the command queue — from a test that never mentioned filament. The failure then
   * lands two files away, on whichever test asserts that the queue starts empty, and the
   * suite hangs after it. Defect #48; it was latent for as long as no test between the
   * heater tests and the queue tests happened to idle.
   */
  if (print_job_timer.isRunning() || print_job_timer.isPaused()) print_job_timer.stop();

  /**
   * Leave nothing queued to run, either.
   *
   * The block buffer below is motion the machine still intends to make; this is the same
   * thing one level up — commands it still intends to *read*. A leftover command runs at
   * some arbitrary later point, inside a test that did not ask for it, and the injection
   * queue is worse than the ring buffer because nothing in the report names it — which is
   * why the two injection slots are cleared by hand: `GCodeQueue::clear()` empties only
   * the ring buffer.
   */
  queue.clear();
  queue.injected_commands_P = nullptr;
  queue.injected_commands[0] = '\0';

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
   * Leave the machine's origin where it was found.
   *
   * A home offset shifts every coordinate the firmware reports and acts on, so a test that sets
   * one and then *fails* hands every later test a machine whose idea of zero has moved. This is
   * register #47, and it was recorded there as a fixture problem; it is not. A scope guard
   * cannot fix it, because Unity's failure path is the `longjmp` described above and destructors
   * do not run — which is precisely the case where an offset is most likely to be left behind.
   *
   * Added defensively rather than in response to a diagnosed failure, and the distinction is
   * worth recording because it was got wrong here first. A failing home-offset test *did* take
   * the suite down — 818 tests became 490 — and the leaked offset was the obvious suspect. It
   * was not the cause: the cause was `SerialCapture`'s drainer thread writing into a discarded
   * stack frame, and this restore changed nothing about it. A plausible mechanism that fits the
   * evidence is not a diagnosis; the backtrace was, and it named something else entirely.
   *
   * Zero is the power-on state — `home_offset` is a zeroed static and `settings.reset()` puts it
   * back there — so this is the same restore as the heater targets rather than a value invented
   * here. Tests that are *about* home offsets set their own and are unaffected.
   */
  #if HAS_HOME_OFFSET
    LOOP_NUM_AXES(i) motion.set_home_offset(AxisEnum(i), 0);
  #endif

  /**
   * ...and leave nothing claiming to be paused.
   *
   * `did_pause_print` is a mode rather than a value: while it is non-zero the machine reports
   * itself paused and the next `pause_print()` anywhere returns false without doing anything.
   * A test that pauses and then fails leaks that to every test after it, and the damage is a
   * *silent no-op* rather than a failure — the later test pauses, nothing happens, and its
   * assertions quietly describe a machine that never moved.
   */
  #if ENABLED(ADVANCED_PAUSE_FEATURE)
    did_pause_print = 0;
  #endif

  /**
   * ...and nothing left claiming an emergency.
   *
   * The emergency parser's flags are latches: the queue reads `killed_by_M112` and halts the
   * machine, and nothing clears it but the code that acts on it. A test that raises one and then
   * fails skips its own cleanup, and every later test runs against a machine that has been told
   * to stop — which presents as the suite grinding to a halt rather than as a failure, because a
   * halted machine still answers.
   *
   * Found exactly that way: one failing assertion in the parser tests took the run from 759 tests
   * in 13 s to 186 in eleven minutes.
   */
  #if ENABLED(EMERGENCY_PARSER)
    EmergencyParser::killed_by_M112 = false;
    EmergencyParser::quickstop_by_M410 = false;
    TERN_(HAS_MEDIA, EmergencyParser::sd_abort_by_M524 = false);
    EmergencyParser::enable();
  #endif

  /**
   * ...and put the filament sensors back to the state they power up in.
   *
   * A simulated pin reads LOW at reset, which `FIL_RUNOUT_STATE` defines as *no filament*, and
   * tests that need a loaded machine drive them the other way. Left driven, the next test to ask
   * the sensor a question gets the previous test's answer — which is how
   * `runout___poll_runout_states` came to expect 7 and read 0, two files away from the test that
   * moved them.
   *
   * Reset rather than saved-and-restored: LOW is what the hardware gives at power-on, and a test
   * that wants filament says so itself.
   */
  #if HAS_FILAMENT_SENSOR
    WRITE(FIL_RUNOUT_PIN, FIL_RUNOUT_STATE);
    #if NUM_RUNOUT_SENSORS >= 2
      WRITE(FIL_RUNOUT2_PIN, FIL_RUNOUT_STATE);
    #endif
    #if NUM_RUNOUT_SENSORS >= 3
      WRITE(FIL_RUNOUT3_PIN, FIL_RUNOUT_STATE);
    #endif
    runout.reset();
  #endif

  /**
   * Leave the machine the size it was.
   *
   * Steps-per-millimetre is the scale everything else is expressed in: it decides how many
   * pulses a move costs and therefore how long every later test takes. A test that changes
   * it and then fails never reaches its own restore — Unity's failure path is the `longjmp`
   * described above, so neither the tail of the test nor any destructor runs.
   *
   * The consequence is not a failure in the next test, it is a suite that stops finishing.
   * Leaving X at a resolution an order of magnitude out turned an eight-second run into one
   * that had to be killed at fifteen minutes, with every test still passing on the way. That
   * is worse than a wrong answer, because there is nothing in the output to read.
   *
   * Restored through the setter, so the reciprocal cache comes back with it.
   */
  if (!steps_per_mm_recorded) {
    bool all_real = true;
    LOOP_DISTINCT_AXES(i) if (planner.steps_per_mm(AxisEnum(i)) <= 0) all_real = false;
    if (all_real) {
      LOOP_DISTINCT_AXES(i) settled_steps_per_mm[i] = planner.steps_per_mm(AxisEnum(i));
      steps_per_mm_recorded = true;
    }
  }
  else LOOP_DISTINCT_AXES(i)
    if (planner.steps_per_mm(AxisEnum(i)) != settled_steps_per_mm[i])
      planner.set_steps_per_mm(AxisEnum(i), settled_steps_per_mm[i]);

  /**
   * Leave the origin where it was.
   *
   * The home offset shifts the whole coordinate system at the next home, so a test that
   * sets one and then fails hands every later test a machine whose origin has moved. The
   * symptom lands nowhere near the cause: one M428 assertion failed under
   * `012-max_endstops`, and four tests later `G28_puts_the_origin_wherever_the_switch_is`
   * expected `X_MIN_POS` and got -5, with nothing wrong with it.
   *
   * Same `longjmp` as the heater targets and the resolution above, and the same answer.
   * Zero rather than a recorded baseline, because zero is what `Motion::home_offset` is
   * defined as and no fixture here establishes another.
   */
  #if HAS_HOME_OFFSET
    LOOP_NUM_AXES(i)
      if (motion.home_offset[i] != 0) motion.set_home_offset(AxisEnum(i), 0);
  #endif

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
