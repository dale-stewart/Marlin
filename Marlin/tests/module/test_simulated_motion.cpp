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
 * Tests for a machine that actually moves.
 *
 * Everything here runs the real stepper interrupt against the real planner, so the
 * assertions are about steps taken rather than about intentions recorded. See
 * tests/support/simulated_machine.h.
 */

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"

MARLIN_TEST(simulated_motion, a_planned_move_steps_the_motor) {
  SimulatedMachine machine;

  xyze_pos_t origin = { 0 };
  planner.set_position_mm(origin);

  xyze_pos_t target = { 0 };
  target.x = 1.0f;
  TEST_ASSERT_TRUE(planner.buffer_line(target, 10.0f));
  TEST_ASSERT_TRUE(planner.has_blocks_queued());

  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  // One millimetre at 80 steps/mm is 80 steps. Not "about 80".
  TEST_ASSERT_EQUAL(80, stepper.position(X_AXIS));
  TEST_ASSERT_FALSE(planner.has_blocks_queued());
}

MARLIN_TEST(simulated_motion, the_distance_moved_matches_what_was_asked_for) {
  SimulatedMachine machine;

  xyze_pos_t origin = { 0 };
  planner.set_position_mm(origin);

  xyze_pos_t target = { 0 };
  target.x = 12.5f;
  target.y = 4.0f;
  planner.buffer_line(target, 10.0f);
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_EQUAL_FLOAT(12.5f, SimulatedMachine::stepper_mm(X_AXIS));
  TEST_ASSERT_EQUAL_FLOAT(4.0f, SimulatedMachine::stepper_mm(Y_AXIS));
}

MARLIN_TEST(simulated_motion, moving_back_returns_to_where_it_started) {
  SimulatedMachine machine;

  xyze_pos_t origin = { 0 };
  planner.set_position_mm(origin);

  xyze_pos_t there = { 0 }; there.x = 5.0f;
  planner.buffer_line(there, 10.0f);
  SimulatedMachine::run_until_idle();
  TEST_ASSERT_EQUAL(400, stepper.position(X_AXIS));

  planner.buffer_line(origin, 10.0f);
  SimulatedMachine::run_until_idle();
  TEST_ASSERT_EQUAL(0, stepper.position(X_AXIS));
}

// Several queued moves all get run, in order, and the total is what was asked for.
MARLIN_TEST(simulated_motion, a_queue_of_moves_all_run) {
  SimulatedMachine machine;

  xyze_pos_t p = { 0 };
  planner.set_position_mm(p);

  for (uint8_t i = 1; i <= 3; i++) {
    p.x = float(i);
    planner.buffer_line(p, 10.0f);
  }
  TEST_ASSERT_TRUE(planner.has_blocks_queued());
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_EQUAL(240, stepper.position(X_AXIS));    // 3 mm
}

// A move that has not been run yet has taken no steps: the queue really does defer.
MARLIN_TEST(simulated_motion, a_queued_move_has_not_moved_anything_yet) {
  SimulatedMachine machine;

  xyze_pos_t origin = { 0 };
  planner.set_position_mm(origin);
  const int32_t before = stepper.position(X_AXIS);

  xyze_pos_t target = { 0 }; target.x = 10.0f;
  planner.buffer_line(target, 10.0f);
  TEST_ASSERT_EQUAL(before, stepper.position(X_AXIS));

  SimulatedMachine::run_until_idle();
  TEST_ASSERT_EQUAL(800, stepper.position(X_AXIS));
}

//
// ---- Which way each motor turns ----
//

namespace {

  // The level each direction pin is left at after a small move with the given signs.
  //
  // Read as a set, and only ever compared with another set from the same axis: the level that
  // means "forwards" depends on how the motor is wired (`INVERT_*_DIR`), so an assertion on
  // the level itself would be asserting the wiring. An assertion that two moves differ is
  // about the firmware.
  struct DirectionPins {
    uint16_t x, y, z;
  };

  DirectionPins direction_after(const float dx, const float dy, const float dz) {
    xyze_pos_t origin = { 0 };
    planner.set_position_mm(origin);
    xyze_pos_t target = { 0 };
    NUM_AXIS_CODE(target.x = dx, target.y = dy, target.z = dz, , , , , , );
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(target, 10.0f), "the move was not accepted");
    TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the move never finished");
    return { Gpio::get(X_DIR_PIN), Gpio::get(Y_DIR_PIN), Gpio::get(Z_DIR_PIN) };
  }

}

/**
 * A motor turns the other way for a move the other way.
 *
 * The direction pin is the whole of what tells a stepper driver which way to go, and it is
 * decided once per move from the sign of that axis's displacement. Getting it wrong on one
 * axis mirrors the print in that axis, which is not subtle but is also not something any
 * assertion about *step counts* would ever notice: the same number of steps is taken either
 * way.
 *
 * Each axis is compared only against itself, so nothing here depends on which level means
 * forwards on this machine.
 */
MARLIN_TEST(simulated_motion, each_motor_reverses_when_its_axis_reverses) {
  SimulatedMachine machine;

  TEST_ASSERT_TRUE_MESSAGE(direction_after(1.0f, 0, 0).x != direction_after(-1.0f, 0, 0).x,
    "X should drive its direction pin oppositely for opposite moves");
  #if HAS_Y_AXIS
    TEST_ASSERT_TRUE_MESSAGE(direction_after(0, 1.0f, 0).y != direction_after(0, -1.0f, 0).y,
      "Y should drive its direction pin oppositely for opposite moves");
  #endif
  #if HAS_Z_AXIS
    TEST_ASSERT_TRUE_MESSAGE(direction_after(0, 0, 1.0f).z != direction_after(0, 0, -1.0f).z,
      "Z should drive its direction pin oppositely for opposite moves");
  #endif
}

/**
 * A positive move ends up further along, on every axis.
 *
 * The test above compares each axis with itself and so cannot see an axis that is reversed
 * *consistently* — both directions flip together and the two still differ. That is not a
 * hypothetical: inverting Z sends the nozzle into the bed on the first move, and it is
 * exactly the mutation that survived a suite asserting only that opposite moves differ.
 *
 * What pins it is the signed position the steppers actually reach, which is counted from the
 * direction pin and the step pulses rather than from the plan. Asked to go forwards, the
 * machine must end up forwards.
 */
MARLIN_TEST(simulated_motion, a_positive_move_leaves_every_axis_further_along) {
  SimulatedMachine machine;

  xyze_pos_t origin = { 0 };
  planner.set_position_mm(origin);

  xyze_pos_t target = { 0 };
  NUM_AXIS_CODE(target.x = 1.0f, target.y = 1.0f, target.z = 1.0f, , , , , , );
  TEST_ASSERT_TRUE(planner.buffer_line(target, 5.0f));
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  const int32_t steps = int32_t(SimulatedMachine::STEPS_PER_MM);
  TEST_ASSERT_EQUAL_MESSAGE(steps, stepper.position(X_AXIS), "X should have gone forwards");
  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_MESSAGE(steps, stepper.position(Y_AXIS), "Y should have gone forwards");
  #endif
  #if HAS_Z_AXIS
    TEST_ASSERT_EQUAL_MESSAGE(steps, stepper.position(Z_AXIS), "Z should have gone forwards, not into the bed");
  #endif

  // ...and back again, so the reverse is pinned too rather than assumed symmetric.
  TEST_ASSERT_TRUE(planner.buffer_line(origin, 5.0f));
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
  TEST_ASSERT_EQUAL_MESSAGE(0, stepper.position(X_AXIS), "X should have come back");
  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_MESSAGE(0, stepper.position(Y_AXIS), "Y should have come back");
  #endif
  #if HAS_Z_AXIS
    TEST_ASSERT_EQUAL_MESSAGE(0, stepper.position(Z_AXIS), "Z should have come back");
  #endif
}

#if HAS_Y_AXIS && HAS_Z_AXIS

/**
 * Each motor takes its direction from its own axis and no other.
 *
 * A diagonal move carries a different sign on each axis at once, which is where a mixed-up
 * assignment shows: swap two of them and every straight move still comes out right, because
 * a move along one axis alone has nothing to be confused with. The comparison is against the
 * single-axis move of the same sign, so each pin is checked against what that axis alone
 * would have produced.
 */
MARLIN_TEST(simulated_motion, each_motor_takes_its_direction_from_its_own_axis) {
  SimulatedMachine machine;

  const DirectionPins x_alone = direction_after(1.0f, 0, 0),
                      y_alone = direction_after(0, -1.0f, 0),
                      z_alone = direction_after(0, 0, 1.0f),
                      together = direction_after(1.0f, -1.0f, 1.0f);

  TEST_ASSERT_EQUAL_MESSAGE(x_alone.x, together.x,
    "X should turn the way its own displacement says, whatever Y and Z are doing");
  TEST_ASSERT_EQUAL_MESSAGE(y_alone.y, together.y,
    "and Y the way its own says");
  TEST_ASSERT_EQUAL_MESSAGE(z_alone.z, together.z,
    "and Z the way its own says");

  // The three are not all the same signal: one axis is going the other way, so at least one
  // pin must differ from the others in the way the single-axis moves differ too.
  TEST_ASSERT_TRUE_MESSAGE(direction_after(0, 1.0f, 0).y != y_alone.y,
    "the Y move used here must be a real reversal, or the assertions above are vacuous");
}

#endif // HAS_Y_AXIS && HAS_Z_AXIS
