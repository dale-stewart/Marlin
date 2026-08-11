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
 * Changing tools on a machine with more than one nozzle.
 *
 * Two nozzles bolted to the same carriage are not in the same place, and the machine only has
 * one set of coordinates. So a tool change is mostly a change of *coordinate system*: after it,
 * `X50` has to put the newly selected nozzle at 50, which means the machine's idea of where it
 * is must shift by the difference between the two nozzles' offsets. Get the sign wrong and
 * every coordinate after the first tool change is out by twice the offset.
 *
 * None of this file runs in the default configuration, which has one extruder — `tool_change.cpp`
 * is not even compiled there, which is why it showed 0% for so long. It runs under
 * `test/003-extruders_3_runout.ini`, and any mutation figure for `tool_change.cpp` has to name
 * that configuration or it is measuring a file that is not in the build.
 *
 * Three extruders rather than two, deliberately: with two, "the other tool" and "tool 1" are the
 * same thing, and an implementation that toggled instead of selecting would pass.
 */

#include "src/inc/MarlinConfig.h"

#if defined(__PLAT_TEST__) && HAS_MULTI_EXTRUDER

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../gcode/serial_capture.h"
#include "../support/step_order.h"
#include "../support/simulated_endstops.h"
#include "src/module/stepper.h"
#include "src/module/tool_change.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

#include <string.h>
#include <string>

namespace {

  // Where the machine sits for these tests: well inside every limit, so nothing here is really
  // about clamping, and clear of the bed so a tool change has room to raise Z if it wants to.
  constexpr float AT_X = 60.0f, AT_Y = 60.0f, AT_Z = 20.0f;

  std::string host_sends(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    return capture.finish();
  }

  /**
   * A machine with three nozzles in three different places, and tool 0 selected.
   *
   * The offsets are stated here rather than taken from the configuration: this build leaves
   * `HOTEND_OFFSET_X/Y` commented out, so every nozzle sits at the origin and the shift under
   * test would be zero — a test that asserted "the coordinates moved by the offset" would pass
   * against a machine that never moved them at all.
   *
   * They are also restored, because `hotend_offset` outlives the test that set it and `M218`
   * is the command a later test would find already applied.
   */
  struct ThreeNozzles {
    SimulatedMachine machine;
    xyz_pos_t was[EXTRUDERS];
    uint8_t was_tool;

    ThreeNozzles() {
      was_tool = motion.extruder;
      for (uint8_t e = 0; e < EXTRUDERS; e++) was[e] = motion.hotend_offset[e];

      // Tool 0 defines the coordinate system, so its offset is zero by convention. The others
      // are deliberately different in both axes and unequal to each other.
      motion.hotend_offset[0].reset();
      #if EXTRUDERS > 1
        motion.hotend_offset[1].set(12.0f, 4.0f, 0.0f);
      #endif
      #if EXTRUDERS > 2
        motion.hotend_offset[2].set(-7.0f, 9.0f, 0.0f);
      #endif

      motion.extruder = 0;
      standing_at(AT_X, AT_Y, AT_Z);
    }

    ~ThreeNozzles() {
      for (uint8_t e = 0; e < EXTRUDERS; e++) motion.hotend_offset[e] = was[e];
      motion.extruder = was_tool;
      planner.clear_block_buffer();
    }

    static void standing_at(const float x, const float y, const float z) {
      xyze_pos_t at = motion.position;
      at.x = x; at.y = y; at.z = z;
      motion.position = at;
      planner.set_position_mm(at);
      motion.destination = at;
    }
  };

}

/**
 * Selecting a tool that is not there is refused, and changes nothing.
 *
 * A corrupt line or a file sliced for a bigger machine asks for a tool this one does not have.
 * Carrying on with it would index past the end of every per-tool table there is — offsets,
 * temperatures, flow rates — so the request is rejected and the machine stays on the tool it
 * was using.
 */
MARLIN_TEST(tool_change, a_tool_the_machine_does_not_have_is_refused) {
  ThreeNozzles nozzles;

  char cmd[8];
  snprintf(cmd, sizeof(cmd), "T%u", unsigned(EXTRUDERS));   // one past the last real tool
  const std::string said = host_sends(cmd);

  TEST_ASSERT_TRUE_MESSAGE(said.find(STR_INVALID_EXTRUDER) != std::string::npos,
    "asking for a tool that does not exist should say so");
  TEST_ASSERT_EQUAL_MESSAGE(0, motion.extruder,
    "and should leave the machine on the tool it was already using");
}

/**
 * Changing tools moves the coordinate system by the distance between the nozzles.
 *
 * The carriage has not gone anywhere — what changed is which nozzle the coordinates refer to.
 * A nozzle mounted 12 mm further along X than the first means that, with the carriage where it
 * is, the newly selected nozzle is 12 mm further along than the old one was; so the machine's
 * position must read 12 mm more than it did.
 *
 * Asserted as the shift being exactly the offset difference, in both axes at once, so a change
 * that applied the offset to one axis or halved it is visible.
 */
MARLIN_TEST(tool_change, changing_tools_shifts_the_coordinates_by_the_nozzle_offset) {
  ThreeNozzles nozzles;

  const xyz_pos_t before = motion.position;
  const xyz_pos_t expected_shift = motion.hotend_offset[1] - motion.hotend_offset[0];

  TEST_ASSERT_TRUE_MESSAGE(expected_shift.x != 0.0f && expected_shift.y != 0.0f,
    "this test needs two nozzles that differ in both axes");

  host_sends("T1 S1");   // S1: change the tool without moving the carriage back

  TEST_ASSERT_EQUAL_MESSAGE(1, motion.extruder, "the tool should have changed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.x + expected_shift.x, motion.position.x,
    "X should have moved by the difference between the two nozzles");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.y + expected_shift.y, motion.position.y,
    "and Y likewise");
}

/**
 * ...and each tool gets its own offset, not a fixed one.
 *
 * The second nozzle is 12 mm one way, the third 7 mm the other. A machine that applied one
 * constant, or that toggled between two, would satisfy the test above; this needs the offset
 * to be looked up per tool. The third nozzle's X offset is negative for the same reason.
 */
MARLIN_TEST(tool_change, each_tool_brings_its_own_offset) {
  ThreeNozzles nozzles;

  const xyz_pos_t before = motion.position;
  const xyz_pos_t to_third = motion.hotend_offset[2] - motion.hotend_offset[0];

  host_sends("T2 S1");

  TEST_ASSERT_EQUAL_MESSAGE(2, motion.extruder, "the third tool should have been selected");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.x + to_third.x, motion.position.x,
    "the third nozzle's own X offset should have been applied");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.y + to_third.y, motion.position.y,
    "and its own Y offset");
}

/**
 * Going out and back leaves the coordinates where they started.
 *
 * The relation rather than the numbers: whatever the offsets are, the shift applied on the way
 * to a tool has to be undone on the way back, or a job that changes tools repeatedly walks the
 * coordinate system further out with every change. Two changes via a third tool, so this is not
 * satisfied by an implementation that simply remembers the position it had before the last one.
 */
MARLIN_TEST(tool_change, changing_away_and_back_leaves_the_coordinates_where_they_started) {
  ThreeNozzles nozzles;

  const xyz_pos_t before = motion.position;

  host_sends("T1 S1");
  host_sends("T2 S1");
  host_sends("T0 S1");

  TEST_ASSERT_EQUAL_MESSAGE(0, motion.extruder, "the machine should be back on the first tool");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.x, motion.position.x,
    "a round trip through the tools should leave X where it started");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before.y, motion.position.y,
    "and Y where it started");
}

/**
 * The planner is told about the shift too.
 *
 * `motion.position` is the firmware's idea of where the tool is; the planner keeps its own, in
 * steps, and the next move is planned as the difference between them. If only one of the two is
 * shifted, the first move after a tool change travels by the offset as well as by the distance
 * asked for — which is the offset applied twice, once silently.
 */
MARLIN_TEST(tool_change, the_planner_agrees_with_the_machine_after_a_tool_change) {
  ThreeNozzles nozzles;

  host_sends("T1 S1");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, motion.position.x, planner.get_axis_position_mm(X_AXIS),
    "the planner should have been told the new position in X");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, motion.position.y, planner.get_axis_position_mm(Y_AXIS),
    "and in Y");
}

/**
 * A tool change that is allowed to move puts the new nozzle where the old one was.
 *
 * Every test above says `S1`, which changes the coordinate system and leaves the carriage
 * alone. Without it the machine goes back to the position it was at — and because the
 * coordinates now refer to a different nozzle, going back to the same *coordinates* means
 * physically moving the carriage by the distance between the two nozzles, the other way.
 *
 * That is the whole point of the exercise: the print carries on from where it left off, with a
 * different nozzle in the same place. So this asserts both halves — the coordinates come back
 * to where they were, and the carriage really did move to make that true.
 */
MARLIN_TEST(tool_change, a_tool_change_that_may_move_puts_the_new_nozzle_where_the_old_one_was) {
  ThreeNozzles nozzles;

  // A real carriage, counting step pulses. `stepper.position()` cannot be used to measure this:
  // the tool change calls `sync_plan_position()`, which re-references the firmware's step
  // counters to the shifted coordinate without the carriage having moved, so the counter ends
  // up back where it started and reports no movement at all. The simulated rail counts pulses
  // on the pin and is not re-referenced by anything — the same reason `simulated_endstops.h`
  // gives for driving the bed surface from it rather than from `stepper.position()`.
  SimulatedAxisWithLimit rail(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                              X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                              int32_t(-1000.0f * SimulatedMachine::STEPS_PER_MM),
                              int32_t(AT_X * SimulatedMachine::STEPS_PER_MM));

  const xyz_pos_t before = motion.position;
  const xyz_pos_t shift = motion.hotend_offset[1] - motion.hotend_offset[0];
  const int32_t carriage_before = rail.position();

  host_sends("T1");
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the tool change never finished");

  TEST_ASSERT_EQUAL_MESSAGE(1, motion.extruder, "the tool should have changed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, before.x, motion.position.x,
    "the machine should be back at the coordinates it started from");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, before.y, motion.position.y, "in Y as well");

  // ...which it can only be by having moved the carriage the other way by the offset.
  const float carriage_moved_mm =
    float(rail.position() - carriage_before) / SimulatedMachine::STEPS_PER_MM;
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, -shift.x, carriage_moved_mm,
    "the carriage should have moved by the nozzle offset, in the opposite direction");
}

/**
 * A machine that does not know where it is changes tools without moving.
 *
 * Every move a tool change makes is to a coordinate, and on an unhomed machine a coordinate
 * means nothing — the carriage could be anywhere, so parking it or returning it is as likely to
 * drive it into a limit as anywhere else. The tool still changes; only the movement is dropped.
 *
 * Asserted on steps rather than on the position, because the position is shifted by the nozzle
 * offset either way and so cannot say whether anything actually moved.
 */
MARLIN_TEST(tool_change, an_unhomed_machine_changes_tools_without_moving) {
  ThreeNozzles nozzles;

  motion.set_axis_never_homed(X_AXIS);
  TERN_(HAS_Y_AXIS, motion.set_axis_never_homed(Y_AXIS));
  TERN_(HAS_Z_AXIS, motion.set_axis_never_homed(Z_AXIS));
  TEST_ASSERT_TRUE_MESSAGE(motion.homing_needed(), "this test needs a machine that has not homed");

  StepOrder order;
  host_sends("T1");
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the tool change never finished");

  TEST_ASSERT_EQUAL_MESSAGE(1, motion.extruder, "the tool should still have changed");
  TEST_ASSERT_FALSE_MESSAGE(order.x.moved() || order.z.moved(),
    "an unhomed machine should not move the carriage to change tools");
}

#endif // __PLAT_TEST__ && HAS_MULTI_EXTRUDER
