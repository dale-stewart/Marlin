/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2024 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
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
 * Interrupting a print and going back to it.
 *
 * `pause_print()` and `resume_print()` are what `M600` and `M125` are made of: park the nozzle
 * away from the part, let somebody change the filament, then put the tool back exactly where it
 * was and carry on. The file was at **0%** — compiled by two configurations and never once
 * executed — which is the worst place for that to be true, because every failure mode here
 * destroys a print that is already hours old:
 *
 *   - come back to the wrong coordinates and the rest of the object prints offset;
 *   - forget to restore the extruder position and the layer under the nozzle is starved or
 *     flooded;
 *   - fail to park and the nozzle sits melting a hole in the part while the operator works.
 *
 * **`resume_position` is file-static**, so none of this can be asserted by reading the value the
 * code saved. That is the right constraint rather than an obstacle: what matters is not that a
 * number was stored but that the tool *comes back*, and the test says so by moving the machine,
 * pausing it, and looking at where it ends up.
 *
 * **Every argument to `resume_print()` is stated, and that is not tidiness.** Its defaults are
 * written for a machine with somebody standing at it, and three of them will stop a test dead:
 *
 *   - `show_lcd` defaults to *true*, which opens the "Purge More / Resume" menu and then spins on
 *     `while (pause_menu_response == PAUSE_RESPONSE_WAIT_FOR) idle_no_sleep()`. On this build the
 *     menu exists because `EMERGENCY_PARSER` and `HOST_PROMPT_SUPPORT` are both on — the answer
 *     is expected to come from the *host*, and a test that is not standing in for one waits for
 *     ever. Answering it as a host would is a worthwhile test; it is a different one.
 *   - `purge_length` defaults to `ADVANCED_PAUSE_PURGE_LENGTH`, 50 mm at 3 mm/s, which is sixteen
 *     seconds of simulated extrusion these tests have no use for.
 *   - `pause_for_user` reaches the "insert filament and press the button" wait.
 *
 * The general shape: **this file is mostly waits for a person**, so a test of it has to say which
 * of them it is not exercising. The load and purge lengths are stated at every call rather than
 * left to default.
 * `resume_print()`'s purge defaults to `ADVANCED_PAUSE_PURGE_LENGTH`, 50 mm at 3 mm/s — sixteen
 * seconds of simulated extrusion these tests have no use for, and a test that inherits a
 * configuration constant it does not care about is a test whose runtime moves when somebody
 * tunes their printer.
 */

#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if ENABLED(ADVANCED_PAUSE_FEATURE)

#include "../support/simulated_machine.h"
#include "../gcode/simulated_sensors.h"
#include "../gcode/serial_capture.h"
#include "src/feature/pause.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"
#include "src/module/printcounter.h"
#include "src/feature/runout.h"
#include "src/MarlinCore.h"
#include <string>

namespace {


  constexpr celsius_t PRINTING_TEMP = 210;

  /**
   * A nozzle sitting *at* its target, not merely above it.
   *
   * `ensure_safe_temperature(false)` — which `resume_print()` reaches through
   * `load_filament()` — spins on `while (is_heating() && |current - target| > TEMP_WINDOW)
   * idle()`, and `TEMP_WINDOW` is one degree. A fixture that pins the reading anywhere outside
   * that window never satisfies the condition and the suite hangs rather than fails: the first
   * draft of this file held the sensor at 220 against a target of 210 and spun at 100% CPU for
   * ten minutes before it was killed.
   *
   * The reading is pinned equal to the target so the window is entered and stays entered. Time
   * is advanced afterwards because a changed reading takes about 300 ms of simulated time to
   * arrive through the oversampled ADC.
   */
  void a_nozzle_settled_at_temperature() {
    SimulatedSensors::hotend_reads(float(PRINTING_TEMP));
    thermalManager.setTargetHotend(PRINTING_TEMP, 0);
    for (uint16_t i = 0; i < 500; i++) { HAL_test_advance_millis(1); thermalManager.task(); }
  }


  /**
   * Filament in the sensor, and the runout latch cleared.
   *
   * Not optional, and the reason is a trap worth stating: **being paused is itself what arms the
   * runout monitor.** `should_monitor_runout()` is `did_pause_print || printingIsActive()`, so
   * the moment `pause_print()` succeeds the sensor starts being believed — and a simulated pin
   * reads LOW at reset, which `FIL_RUNOUT_STATE` defines as *no filament*.
   *
   * What that produced was not a failure but a stall, and the shape of it is the finding:
   * `resume_print()` purges through `planner.synchronize()`, `synchronize()` spins on `idle()`,
   * `idle()` runs the runout monitor, and the monitor responds by injecting the runout script and
   * calling `planner.synchronize()` *again* — from inside the one it was called from. The stack
   * is `synchronize -> idle -> run -> synchronize -> idle`, and the run never finished. It took a
   * backtrace to see; no amount of reading the test would have shown it.
   */
  void filament_is_loaded() {
    #if HAS_FILAMENT_SENSOR
      WRITE(FIL_RUNOUT_PIN, !FIL_RUNOUT_STATE);
      #if NUM_RUNOUT_SENSORS >= 2
        WRITE(FIL_RUNOUT2_PIN, !FIL_RUNOUT_STATE);
      #endif
      #if NUM_RUNOUT_SENSORS >= 3
        WRITE(FIL_RUNOUT3_PIN, !FIL_RUNOUT_STATE);
      #endif
      runout.reset();
    #endif
  }

  /**
   * Close together, and far enough apart to tell.
   *
   * Every millimetre here is simulated travel that the suite has to step through, and these four
   * tests each move the tool four times — to the print position, to the park point, back in XY,
   * back in Z. The first draft printed at (90, 75) and parked at (20, 30), which is ~90 mm of
   * travel per leg and turned a twelve-second suite into a four-minute one. The distances only
   * have to exceed the tolerance the assertions use.
   */
  constexpr float PARK_X = 12, PARK_Y = 14, PARK_Z_RAISE = 2;
  constexpr float PRINT_X = 30, PRINT_Y = 26, PRINT_Z = 4;

  /**
   * A machine mid-print: homed, hot, and standing over the part.
   *
   * Homed because `pause_print()` refuses to park otherwise — `axes_should_home()` gates it, and
   * a fixture that skipped homing would silently be testing the *refusal* path in every test
   * below while looking like it tested parking.
   *
   * Hot because the initial retract is guarded on `hotEnoughToExtrude()`. That guard has its own
   * test; here the nozzle is brought up to temperature so the ordinary path is the one running.
   */
  void a_machine_printing_at(const float x, const float y, const float z) {
    a_nozzle_settled_at_temperature();
    filament_is_loaded();
    motion.set_all_homed();
    motion.destination.set(x, y, z, motion.position.e);
    motion.prepare_internal_move_to_destination();
    planner.synchronize();
  }

  xyz_pos_t the_park_point() {
    xyz_pos_t p; p.set(PARK_X, PARK_Y, PARK_Z_RAISE); return p;
  }

  /**
   * Puts back everything a pause leaves behind.
   *
   * `did_pause_print` is the one that matters: left non-zero it makes the machine claim to be
   * paused for every later test, and the next `pause_print()` anywhere returns false without
   * doing anything. Unity's failure path skips this destructor, which is why the same reset also
   * lives in `quiesce_simulated_peripherals()` — this is the tidy path, that is the safety net.
   */
  struct PausedMachine {
    celsius_t was_target;
    PausedMachine() : was_target(thermalManager.degTargetHotend(0)) {}
    ~PausedMachine() {
      did_pause_print = 0;
      thermalManager.setTargetHotend(was_target, 0);
      if (print_job_timer.isRunning() || print_job_timer.isPaused()) print_job_timer.stop();
    }
  };

}

/**
 * The tool parks away from the part, and comes back to where it was printing.
 *
 * The whole point of the feature in one assertion pair. The park position is deliberately
 * nowhere near the print position, so "it came back" cannot be satisfied by a machine that
 * never moved — and the parked position is asserted in between, so it cannot be satisfied by a
 * machine that failed to park either.
 *
 * Z is checked separately from XY because they are restored by two different moves in
 * `resume_print()`, in a deliberate order: XY first at the parked height, then Z down. A version
 * that lowered Z first would drag the nozzle across the part on the way back, which is exactly
 * the accident parking exists to avoid.
 */
MARLIN_TEST(pause, the_tool_parks_and_then_returns_to_where_it_was_printing) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  PausedMachine paused;

  a_machine_printing_at(PRINT_X, PRINT_Y, PRINT_Z);

  TEST_ASSERT_TRUE_MESSAGE(pause_print(0, the_park_point()),
    "a running machine should accept a pause");
  planner.synchronize();

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, PARK_X, motion.position.x,
    "the nozzle should have parked in X, away from the part");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, PARK_Y, motion.position.y,
    "and in Y");
  TEST_ASSERT_TRUE_MESSAGE(motion.position.z > PRINT_Z,
    "and should have lifted clear of the part rather than sliding across it");

  resume_print(0, 0, 0, 0, 0, /*show_lcd=*/false);
  planner.synchronize();

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, PRINT_X, motion.position.x,
    "resuming should bring the tool back to where the print was interrupted, in X");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, PRINT_Y, motion.position.y,
    "and in Y");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, PRINT_Z, motion.position.z,
    "and back down to the layer it was on");
}

/**
 * A second pause does not overwrite the first one's saved position.
 *
 * `pause_print()` opens with `if (did_pause_print) return false`, and it would be easy to read
 * that as tidiness. It is not: the line after the guard is `resume_position = motion.position`,
 * and by the time a second pause arrives the machine is *parked*. Without the guard the saved
 * position becomes the park point, and resuming returns the tool there instead of to the print —
 * leaving the rest of the object to be extruded into thin air twenty millimetres away.
 *
 * So the assertion is not that the second call returned false. It is that after two pauses the
 * machine still knows where the print was, which is the consequence the guard exists for and the
 * only thing that would notice if someone "simplified" it.
 */
MARLIN_TEST(pause, a_second_pause_does_not_forget_where_the_print_was) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  PausedMachine paused;

  a_machine_printing_at(PRINT_X, PRINT_Y, PRINT_Z);

  TEST_ASSERT_TRUE_MESSAGE(pause_print(0, the_park_point()), "the first pause should be accepted");
  planner.synchronize();

  TEST_ASSERT_FALSE_MESSAGE(pause_print(0, the_park_point()),
    "a machine already paused should refuse a second pause");
  planner.synchronize();

  resume_print(0, 0, 0, 0, 0, /*show_lcd=*/false);
  planner.synchronize();

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, PRINT_X, motion.position.x,
    "after two pauses the tool should still return to the print, not to the park point");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, PRINT_Y, motion.position.y,
    "in Y as well");
}

/**
 * The clock stops while the machine is waiting for a person, and starts again after.
 *
 * The print job timer is what the operator is told when they ask how long is left, and a pause
 * is not printing time — a filament change can take ten minutes or an afternoon. A timer left
 * running through it makes every estimate for the rest of the job wrong, and worse, it does so
 * silently and in a way that looks like the estimate was simply bad.
 *
 * Both directions, because a machine that never restarted the clock would pass the first
 * assertion perfectly.
 */
MARLIN_TEST(pause, the_job_clock_stops_while_paused_and_runs_again_after) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  PausedMachine paused;

  a_machine_printing_at(PRINT_X, PRINT_Y, PRINT_Z);
  print_job_timer.start();
  TEST_ASSERT_TRUE_MESSAGE(print_job_timer.isRunning(),
    "this test is about a job that was being timed");

  pause_print(0, the_park_point());
  planner.synchronize();

  TEST_ASSERT_FALSE_MESSAGE(print_job_timer.isRunning(),
    "the job clock should not run while the machine is parked waiting for someone");
  TEST_ASSERT_TRUE_MESSAGE(print_job_timer.isPaused(),
    "and it should be paused rather than stopped, so the elapsed time survives");

  resume_print(0, 0, 0, 0, 0, /*show_lcd=*/false);
  planner.synchronize();

  TEST_ASSERT_TRUE_MESSAGE(print_job_timer.isRunning(),
    "and it should be running again once the print carries on");
}

/**
 * A machine that has not homed is not asked to park.
 *
 * Parking means moving to coordinates, and coordinates mean nothing before homing — the counters
 * hold whatever they held at power-on. A pause that parked anyway would drive the nozzle to a
 * position it invented, which on an unhomed machine can be through the bed or into a limit.
 *
 * So the pause still happens — that part is right, the print must stop — but the move does not,
 * and the operator is told the parking failed rather than left to wonder why the nozzle is
 * sitting on the part.
 */
MARLIN_TEST(pause, an_unhomed_machine_pauses_without_parking) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  PausedMachine paused;

  a_nozzle_settled_at_temperature();
  filament_is_loaded();
  motion.set_all_unhomed();
  TEST_ASSERT_TRUE_MESSAGE(motion.axes_should_home(),
    "this test is about a machine that does not know where it is");

  const xyz_pos_t before = motion.position;

  TEST_ASSERT_TRUE_MESSAGE(pause_print(0, the_park_point()),
    "the print should still be paused - stopping is right even when parking is not possible");
  planner.synchronize();

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.x, motion.position.x,
    "but the nozzle must not be sent to coordinates the machine cannot know, in X");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.y, motion.position.y,
    "or in Y");
}

#endif // ADVANCED_PAUSE_FEATURE
