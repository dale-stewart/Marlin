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
 * The order the machine does things in, and how much room it leaves.
 *
 * `blocking_move()` is what the rest of the firmware calls when it wants the tool somewhere
 * and wants to wait until it is there — homing, probing, tool changes, filament changes all
 * end up here. Where it ends up is the easy half; the half that matters is that it raises Z
 * *before* crossing the bed and lowers it only *after* arriving, so the nozzle passes over a
 * printed part rather than through it.
 *
 * Both orderings finish at exactly the same coordinates, so no assertion about the final
 * position can distinguish them. The evidence has to be the sequence of steps, which is what
 * `support/step_order.h` records.
 *
 * The companion subject is `do_z_clearance()`: how high to go so that whatever hangs lowest
 * clears the bed. Its rules are all about *not* moving — it refuses to descend unless told it
 * may, does nothing at all if already at the height, and never asks for more than the machine
 * has. Those are assertions about steps that were not taken, and "no steps" is a stronger
 * claim than "the position is unchanged": a move out and back would satisfy the second.
 *
 * Test-HAL only. These wait for movement, which needs a clock that advances.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_Z_AXIS

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/step_order.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/stepper.h"

namespace {

  // Somewhere in the middle of the bed with room to move in every direction, so nothing here
  // is near a software limit and no assertion is really about clamping.
  constexpr float MID_X = 60.0f, MID_Y = 60.0f;

  // Put the machine somewhere without moving it, and agree with the planner about it.
  void standing_at(const float x, const float y, const float z) {
    xyze_pos_t at = motion.position;
    at.x = x; at.y = y; at.z = z;
    motion.position = at;
    planner.set_position_mm(at);
    motion.destination = at;
  }

  struct ReadyToMove {
    SimulatedMachine machine;
    ReadyToMove() { motion.set_all_homed(); }
    ~ReadyToMove() { planner.clear_block_buffer(); }
  };

}

/**
 * Going somewhere higher, the nozzle lifts before it travels.
 *
 * This is the whole reason `blocking_move` exists rather than a single planner call: a
 * diagonal from here to there would drag the nozzle up through whatever stands between the
 * two points. Lifting first means the crossing happens at the higher of the two heights.
 */
MARLIN_TEST(motion_sequencing, a_move_up_and_across_lifts_before_it_travels) {
  ReadyToMove ready;
  standing_at(MID_X, MID_Y, 5.0f);

  StepOrder order;
  motion.blocking_move(NUM_AXIS_LIST_(MID_X + 20.0f, MID_Y + 20.0f, 25.0f,
                                      motion.position.i, motion.position.j, motion.position.k,
                                      motion.position.u, motion.position.v, motion.position.w)
                       0.0f);

  TEST_ASSERT_TRUE_MESSAGE(order.z.moved(), "Z should have moved: it was asked to go 20 mm higher");
  TEST_ASSERT_TRUE_MESSAGE(order.x.moved(), "X should have moved: it was asked to go 20 mm across");
  TEST_ASSERT_TRUE_MESSAGE(StepOrder::finished_before(order.z, order.x),
    "Z should have finished rising before X began, so the crossing happens up and clear");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 25.0f, motion.position.z, "and should have arrived at the height asked for");
}

/**
 * Going somewhere lower, it travels first and descends at the end.
 *
 * The mirror image, and the half that a machine could get wrong while still passing the test
 * above. Descending first would put the nozzle at the low height for the whole crossing —
 * which is the exact situation the ordering exists to avoid.
 */
MARLIN_TEST(motion_sequencing, a_move_down_and_across_travels_before_it_descends) {
  ReadyToMove ready;
  standing_at(MID_X, MID_Y, 25.0f);

  StepOrder order;
  motion.blocking_move(NUM_AXIS_LIST_(MID_X + 20.0f, MID_Y + 20.0f, 5.0f,
                                      motion.position.i, motion.position.j, motion.position.k,
                                      motion.position.u, motion.position.v, motion.position.w)
                       0.0f);

  TEST_ASSERT_TRUE_MESSAGE(order.z.moved() && order.x.moved(), "both axes were asked to move");
  TEST_ASSERT_TRUE_MESSAGE(StepOrder::finished_before(order.x, order.z),
    "X should have finished crossing before Z began to descend");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 5.0f, motion.position.z, "and should have arrived at the height asked for");
}

/**
 * A clearance move does not descend.
 *
 * `do_z_clearance()` asks for room, and a nozzle already higher than the room asked for has
 * room. Coming down to meet the number would be actively wrong: the caller is about to deploy
 * a probe or cross the bed, and it asked for a floor, not a height.
 */
MARLIN_TEST(motion_sequencing, a_clearance_move_will_not_descend) {
  ReadyToMove ready;
  standing_at(MID_X, MID_Y, 50.0f);

  StepOrder order;
  motion.do_z_clearance(10.0f);

  TEST_ASSERT_FALSE_MESSAGE(order.z.moved(),
    "a nozzle already above the clearance asked for should not move at all");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 50.0f, motion.position.z, "and should still be where it was");
}

/**
 * ...unless the caller says it may.
 *
 * The other arm of the same guard. Some callers do want the nozzle brought to a stated height
 * — after homing Z, say — and they say so. Without this the test above is equally satisfied by
 * a clearance move that never moves anything.
 */
MARLIN_TEST(motion_sequencing, a_clearance_move_descends_when_lowering_is_allowed) {
  ReadyToMove ready;
  standing_at(MID_X, MID_Y, 50.0f);

  StepOrder order;
  motion.do_z_clearance(10.0f, /*with_probe*/ false, /*lower_allowed*/ true);

  TEST_ASSERT_TRUE_MESSAGE(order.z.moved(), "told it may lower, the nozzle should have come down");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 10.0f, motion.position.z, "to the height asked for");
}

/**
 * Asking for the height it is already at moves nothing.
 *
 * Every probe point in a levelling run asks for the same clearance, so this is the common case
 * rather than an edge one, and it is worth stating that the machine stays still for it.
 *
 * What this does **not** test is the `zdest == position.z` guard that appears to implement it.
 * Deleting that guard leaves this test passing, and hand-applying the mutant is how that was
 * found: without it the machine issues a zero-length move, which `MIN_STEPS_PER_SEGMENT`
 * discards before it becomes a block, so no steps are taken either way. The guard saves the
 * work of planning a move that would be thrown away — it has no wrong answer, only a slower
 * one, and no assertion about the machine's behaviour can distinguish it. The mutants on it
 * are recorded as equivalent rather than chased.
 */
MARLIN_TEST(motion_sequencing, asking_for_the_height_it_is_already_at_moves_nothing) {
  ReadyToMove ready;
  constexpr float ASKED_FOR = 12.0f;
  standing_at(MID_X, MID_Y, ASKED_FOR);

  StepOrder order;
  motion.do_z_clearance(ASKED_FOR, /*with_probe*/ false);

  TEST_ASSERT_FALSE_MESSAGE(order.z.moved(),
    "a nozzle already at the clearance height should not be moved to where it already is");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, ASKED_FOR, motion.position.z, "and should still be there");
}

/**
 * Clearance is capped at the top of the machine.
 *
 * A caller asking for more room than the machine has is not refused — it is given all there
 * is. Without the cap the request goes to the planner as written and the carriage drives into
 * the top of the frame, which is the failure the software limits exist to prevent and which
 * nothing downstream of here would catch: `blocking_move` writes the position it was given.
 */
MARLIN_TEST(motion_sequencing, clearance_is_capped_at_the_top_of_the_machine) {
  ReadyToMove ready;
  standing_at(MID_X, MID_Y, 5.0f);

  motion.do_z_clearance((Z_MAX_POS) + 50.0f, /*with_probe*/ false);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, Z_MAX_POS, motion.position.z,
    "more clearance than the machine has should give all of it, not more");
}

/**
 * `do_z_clearance_by` is measured from where the nozzle is.
 *
 * The same call with a relative meaning, and the two are easy to confuse at a call site. Asked
 * for the same number from two different starting heights, it must give two different answers
 * — each one that much above where it began.
 */
MARLIN_TEST(motion_sequencing, clearance_by_an_amount_is_measured_from_the_current_height) {
  ReadyToMove ready;
  constexpr float BY = 7.0f;

  standing_at(MID_X, MID_Y, 5.0f);
  motion.do_z_clearance_by(BY);
  const float from_low = motion.position.z;

  standing_at(MID_X, MID_Y, 30.0f);
  motion.do_z_clearance_by(BY);
  const float from_high = motion.position.z;

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 5.0f + BY, from_low, "seven millimetres above five is twelve");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 30.0f + BY, from_high, "and seven above thirty is thirty-seven");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 25.0f, from_high - from_low,
    "the same relative raise from two heights should differ by the gap between them");
}

#endif // HAS_Z_AXIS
