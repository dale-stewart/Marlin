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
 * Two things a move is not allowed to do to the extruder.
 *
 * Pushing filament through a cold nozzle strips the drive gear or snaps the filament, and a
 * single enormous extrusion is almost always a corrupt line rather than an intention. Both
 * are refused in `prepare_line_to_destination()`, before the move reaches the planner.
 *
 * What makes them interesting to test is *how* they are refused. The move is not abandoned —
 * the machine goes where it was asked to go and behaves as though the E part had happened,
 * because the alternative is that the firmware's idea of the filament position and the host's
 * diverge for the rest of the job. Every subsequent relative extrusion would then be measured
 * from a different place.
 *
 * These need a temperature the test can choose, so they read the hotend through the simulated
 * sensor rather than writing a value into the firmware — see `simulated_sensors.h`.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_EXTRUDERS && ANY(PREVENT_COLD_EXTRUSION, PREVENT_LENGTHY_EXTRUDE)

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../gcode/simulated_sensors.h"
#include "../gcode/serial_capture.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"

#include <string>

namespace {

  // A machine standing still, homed, with the planner agreeing where it is.
  #if ENABLED(PREVENT_COLD_EXTRUSION)
    // The temperature the guard actually compares against. `EXTRUDE_MINTEMP` is the setting;
    // the check allows a window below it, so that a nozzle holding its target does not trip
    // the guard on the low side of its own ripple.
    constexpr celsius_t COLD_BELOW = (EXTRUDE_MINTEMP) - (TEMP_WINDOW);
  #endif

  struct ReadyToMove {
    SimulatedMachine machine;
    SimulatedSensors sensors;
    #if ENABLED(PREVENT_COLD_EXTRUSION)
      bool was_allowed;
    #endif
    float was_e_factor;

    ReadyToMove() {
      // Flow rate scales every extrusion and outlives the `M221` that set it, so the length
      // limit below is measured against a number another test can have changed. Stated here
      // rather than assumed, which is also what makes the flow-rate test's doubling mean
      // something.
      was_e_factor = planner.e_factor[0];
      planner.e_factor[0] = 1.0f;
      // `M302` switches the guard off and the setting outlives the command, so a test that
      // did not state which side it wanted would measure whichever the last one left.
      #if ENABLED(PREVENT_COLD_EXTRUSION)
        was_allowed = thermalManager.allow_cold_extrude;
        thermalManager.allow_cold_extrude = false;
      #endif
      xyze_pos_t here = { 0 }; here.x = 50.0f; here.y = 50.0f; here.z = 10.0f;
      motion.position = here;
      planner.set_position_mm(here);
      motion.set_all_homed();
      motion.destination = here;
    }

    ~ReadyToMove() {
      TERN_(PREVENT_COLD_EXTRUSION, thermalManager.allow_cold_extrude = was_allowed);
      planner.e_factor[0] = was_e_factor;
    }
  };

  // Ask for a move of `de` millimetres of filament, and report what was said about it.
  std::string extrude_by(const float de) {
    motion.destination = motion.position;
    motion.destination.e += de;
    SerialCapture capture;
    motion.prepare_line_to_destination();
    return capture.finish();
  }

}

#if ENABLED(PREVENT_COLD_EXTRUSION)

/**
 * A cold nozzle does not extrude, and says why.
 *
 * The threshold is `EXTRUDE_MINTEMP`, and the sensor is driven to a reading just the wrong
 * side of it rather than to something obviously cold — a test at room temperature would pass
 * against a machine whose threshold was any value at all.
 */
MARLIN_TEST(extrusion_guards, a_cold_hotend_refuses_to_extrude) {
  ReadyToMove ready;

  // The guard compares a whole-degree reading, so the boundary lies between two adjacent
  // degrees: half a degree below is what survives the rounding back up.
  SimulatedSensors::hotend_reads_below(COLD_BELOW - 0.5f);
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.wholeDegHotend(0) < COLD_BELOW,
    "the hotend should read below the temperature the guard compares against");

  const float e_before = motion.position.e;
  const std::string said = extrude_by(5.0f);

  TEST_ASSERT_TRUE_MESSAGE(said.find(STR_ERR_COLD_EXTRUDE_STOP) != std::string::npos,
    "a refused extrusion should say it was refused");
  TEST_ASSERT_FALSE_MESSAGE(planner.has_blocks_queued(),
    "nothing should have been queued for a move that only asked to extrude");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, e_before + 5.0f, motion.position.e,
    "the machine should behave as though the extrusion happened, so later moves still line up");
}

/**
 * A hot nozzle extrudes.
 *
 * The other side of the same threshold, and what stops the test above being satisfied by a
 * machine that never extrudes at all. One reading below the minimum and one at it: any two
 * temperatures further apart would leave the threshold free to sit anywhere between them.
 */
MARLIN_TEST(extrusion_guards, a_hot_enough_hotend_extrudes) {
  ReadyToMove ready;

  SimulatedSensors::hotend_reads_at_least(COLD_BELOW);
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.wholeDegHotend(0) >= COLD_BELOW,
    "the hotend should read at or above the temperature the guard compares against");

  const std::string said = extrude_by(5.0f);

  TEST_ASSERT_TRUE_MESSAGE(said.find(STR_ERR_COLD_EXTRUDE_STOP) == std::string::npos,
    "an allowed extrusion should not be reported as refused");
  TEST_ASSERT_TRUE_MESSAGE(planner.has_blocks_queued(),
    "a hot nozzle should have queued the move");

  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the extrusion move never finished");
}

/**
 * A move that does not touch the extruder is not a cold extrusion.
 *
 * Travel moves happen constantly while the machine is warming up, and refusing them — or
 * reporting them — would make the guard unusable. The check is on the *change* in E, which is
 * what this says.
 */
MARLIN_TEST(extrusion_guards, a_cold_machine_may_still_travel) {
  ReadyToMove ready;

  SimulatedSensors::hotend_reads_below(COLD_BELOW - 0.5f);
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.wholeDegHotend(0) < COLD_BELOW, "this test needs a cold hotend");

  motion.destination = motion.position;
  motion.destination.x += 10.0f;
  SerialCapture capture;
  motion.prepare_line_to_destination();
  const std::string said = capture.finish();

  TEST_ASSERT_TRUE_MESSAGE(said.find(STR_ERR_COLD_EXTRUDE_STOP) == std::string::npos,
    "a move with no extrusion in it should not be refused as a cold extrusion");
  TEST_ASSERT_TRUE_MESSAGE(planner.has_blocks_queued(), "the travel move should have been queued");

  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the travel move never finished");
}

#endif // PREVENT_COLD_EXTRUSION

#if ENABLED(PREVENT_LENGTHY_EXTRUDE)

/**
 * One absurdly long extrusion is refused however hot the nozzle is.
 *
 * `EXTRUDE_MAXLENGTH` is not a temperature question — it is a "this line is probably corrupt"
 * question, and a single command asking for metres of filament is the shape a corrupted
 * coordinate takes. Both sides of the limit are asked for, so the test says where the limit
 * is rather than that some limit exists.
 */
MARLIN_TEST(extrusion_guards, a_single_enormous_extrusion_is_refused) {
  ReadyToMove ready;

  TERN_(PREVENT_COLD_EXTRUSION, SimulatedSensors::hotend_reads_at_least(COLD_BELOW));

  // Just inside the limit: allowed, and no complaint.
  const std::string allowed = extrude_by((EXTRUDE_MAXLENGTH)-1.0f);
  TEST_ASSERT_TRUE_MESSAGE(allowed.find(STR_ERR_LONG_EXTRUDE_STOP) == std::string::npos,
    "an extrusion inside the limit should not be refused");
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the allowed extrusion never finished");

  // Just outside it: refused, and reported.
  const float e_before = motion.position.e;
  const std::string refused = extrude_by((EXTRUDE_MAXLENGTH)+1.0f);
  TEST_ASSERT_TRUE_MESSAGE(refused.find(STR_ERR_LONG_EXTRUDE_STOP) != std::string::npos,
    "an extrusion past the limit should say it was refused");
  TEST_ASSERT_FALSE_MESSAGE(planner.has_blocks_queued(),
    "nothing should have been queued for the refused extrusion");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, e_before + (EXTRUDE_MAXLENGTH) + 1.0f, motion.position.e,
    "a refused extrusion should still leave the machine agreeing with the host about E");
}

/**
 * The limit is on the filament, so a flow-rate change moves it.
 *
 * `M221` scales every extrusion, and the check is made on the scaled amount — the length that
 * would actually be pushed through the nozzle, not the length the file asked for. So an
 * extrusion that is allowed at normal flow is refused at double it.
 */
MARLIN_TEST(extrusion_guards, the_extrusion_limit_is_measured_after_the_flow_rate) {
  ReadyToMove ready;

  TERN_(PREVENT_COLD_EXTRUSION, SimulatedSensors::hotend_reads_at_least(COLD_BELOW));

  // Two thirds of the limit is comfortably allowed...
  const float asked_for = (EXTRUDE_MAXLENGTH) * 2.0f / 3.0f;
  const std::string at_normal_flow = extrude_by(asked_for);
  TEST_ASSERT_TRUE_MESSAGE(at_normal_flow.find(STR_ERR_LONG_EXTRUDE_STOP) == std::string::npos,
    "two thirds of the limit should be allowed at normal flow");
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the allowed extrusion never finished");

  // ...and is half as much again as the limit once doubled.
  planner.e_factor[0] = 2.0f;
  const std::string at_double_flow = extrude_by(asked_for);

  TEST_ASSERT_TRUE_MESSAGE(at_double_flow.find(STR_ERR_LONG_EXTRUDE_STOP) != std::string::npos,
    "the same extrusion at double flow is past the limit and should be refused");
}

#endif // PREVENT_LENGTHY_EXTRUDE

#endif // HAS_EXTRUDERS && ANY(PREVENT_COLD_EXTRUSION, PREVENT_LENGTHY_EXTRUDE)
