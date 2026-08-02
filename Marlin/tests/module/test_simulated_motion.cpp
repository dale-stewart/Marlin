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
