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
 * Tests for coordinate handling and the software limits.
 *
 * The machine has one position; the user has another. Between them sit the workspace
 * offset and the home offset, and getting the conversion wrong sends the tool to the
 * wrong place. The software endstops are the last thing standing between a bad
 * coordinate and the frame.
 *
 * Homing, probing and anything that waits for movement are absent: they need a stepper
 * interrupt that does not run here.
 */

#include "../test/unit_tests.h"
#include "src/module/motion.h"
#include "src/module/stepper.h"

namespace {

  struct SavedOffsets {
    xyz_pos_t was_home;
    #if HAS_WORKSPACE_OFFSET
      xyz_pos_t was_workspace;
    #endif
    SavedOffsets() {
      was_home = motion.home_offset;
      TERN_(HAS_WORKSPACE_OFFSET, was_workspace = motion.workspace_offset);
    }
    ~SavedOffsets() {
      motion.home_offset = was_home;
      TERN_(HAS_WORKSPACE_OFFSET, motion.workspace_offset = was_workspace);
    }
  };

}

#if HAS_WORKSPACE_OFFSET

  MARLIN_TEST(motion, logical_and_native_coordinates_convert_both_ways) {
    SavedOffsets restore;

    motion.workspace_offset.x = 0;
    TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.native_to_logical(10.0f, X_AXIS));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.logical_to_native(10.0f, X_AXIS));

    motion.workspace_offset.x = 5.0f;
    TEST_ASSERT_EQUAL_FLOAT(15.0f, motion.native_to_logical(10.0f, X_AXIS));
    TEST_ASSERT_EQUAL_FLOAT(5.0f, motion.logical_to_native(10.0f, X_AXIS));
  }

  // Converting one way and back must land where it started, whatever the offset.
  MARLIN_TEST(motion, converting_a_coordinate_round_trip_is_lossless) {
    SavedOffsets restore;

    for (const float offset : { -12.5f, 0.0f, 7.25f }) {
      motion.workspace_offset.x = offset;
      for (const float value : { -50.0f, 0.0f, 123.75f }) {
        const float there_and_back =
          motion.logical_to_native(motion.native_to_logical(value, X_AXIS), X_AXIS);
        TEST_ASSERT_EQUAL_FLOAT(value, there_and_back);
      }
    }
  }

#endif

MARLIN_TEST(motion, the_home_offset_can_be_set_per_axis) {
  SavedOffsets restore;

  motion.set_home_offset(X_AXIS, 3.5f);
  TEST_ASSERT_EQUAL_FLOAT(3.5f, motion.home_offset.x);

  motion.set_home_offset(X_AXIS, -1.25f);
  TEST_ASSERT_EQUAL_FLOAT(-1.25f, motion.home_offset.x);
}

#if HAS_SOFTWARE_ENDSTOPS

  /**
   * A coordinate beyond the bed is pulled back to the edge rather than obeyed. This is
   * what stops a bad G-code line driving the carriage into the frame.
   *
   * The limits only apply to an axis that has been homed — until then the machine does
   * not know where it is, so there is nothing to measure a limit against. The tests
   * below say the axes are homed for that reason.
   */
  MARLIN_TEST(motion, a_target_outside_the_bed_is_clamped_to_it) {
    const bool was = motion.soft_endstop._enabled;
    motion.soft_endstop._enabled = true;
    motion.set_all_homed();

    xyze_pos_t target = motion.position;
    target.x = motion.soft_endstop.max.x + 50.0f;
    motion.apply_limits(target);
    TEST_ASSERT_TRUE(target.x <= motion.soft_endstop.max.x);

    target = motion.position;
    target.x = motion.soft_endstop.min.x - 50.0f;
    motion.apply_limits(target);
    TEST_ASSERT_TRUE(target.x >= motion.soft_endstop.min.x);

    motion.soft_endstop._enabled = was;
  }

  MARLIN_TEST(motion, a_target_inside_the_bed_is_left_alone) {
    const bool was = motion.soft_endstop._enabled;
    motion.soft_endstop._enabled = true;
    motion.set_all_homed();

    const float middle = (motion.soft_endstop.min.x + motion.soft_endstop.max.x) / 2.0f;
    xyze_pos_t target = motion.position;
    target.x = middle;
    motion.apply_limits(target);
    TEST_ASSERT_EQUAL_FLOAT(middle, target.x);

    motion.soft_endstop._enabled = was;
  }

  // With the limits switched off, nothing is clamped — which is what M211 S0 is for.
  MARLIN_TEST(motion, limits_switched_off_do_not_clamp) {
    const bool was = motion.soft_endstop._enabled;
    motion.soft_endstop._enabled = false;
    motion.set_all_homed();

    const float beyond = motion.soft_endstop.max.x + 50.0f;
    xyze_pos_t target = motion.position;
    target.x = beyond;
    motion.apply_limits(target);
    TEST_ASSERT_EQUAL_FLOAT(beyond, target.x);

    motion.soft_endstop._enabled = was;
  }

#endif

#if DISABLED(IS_KINEMATIC) && HAS_X_AXIS && HAS_Y_AXIS

/**
 * A point is reachable if it is inside the machine, and not if it is outside.
 *
 * This is what refuses a probe point off the edge of the bed, and it is a decision made
 * before anything moves — so it is asked about coordinates rather than tested by driving at
 * them. `fslop` is the slack allowed at each edge, which exists so a point placed exactly on
 * a limit by arithmetic is not refused for a rounding error.
 *
 * Each of the four bounds is checked from both sides, because a check that only tested one
 * edge would be satisfied by a machine that had no limit in the other direction at all — and
 * the assertions are placed relative to the limits rather than at chosen numbers.
 */
MARLIN_TEST(motion, a_point_inside_the_machine_is_reachable) {
  const float mid_x = (X_MIN_POS + X_MAX_POS) * 0.5f,
              mid_y = (Y_MIN_POS + Y_MAX_POS) * 0.5f;

  TEST_ASSERT_TRUE_MESSAGE(motion.can_reach(mid_x, mid_y), "the middle of the bed should be reachable");
  TEST_ASSERT_TRUE_MESSAGE(motion.can_reach(X_MIN_POS, mid_y), "the X minimum should be reachable");
  TEST_ASSERT_TRUE_MESSAGE(motion.can_reach(X_MAX_POS, mid_y), "the X maximum should be reachable");
  TEST_ASSERT_TRUE_MESSAGE(motion.can_reach(mid_x, Y_MIN_POS), "the Y minimum should be reachable");
  TEST_ASSERT_TRUE_MESSAGE(motion.can_reach(mid_x, Y_MAX_POS), "the Y maximum should be reachable");
}

MARLIN_TEST(motion, a_point_outside_any_bound_is_not_reachable) {
  const float mid_x = (X_MIN_POS + X_MAX_POS) * 0.5f,
              mid_y = (Y_MIN_POS + Y_MAX_POS) * 0.5f;

  // Just past each limit, by enough to clear the slop and no more. A point far outside would
  // pass against a machine whose bound was wrong by a millimetre.
  constexpr float past = 10.0f * fslop;

  TEST_ASSERT_FALSE_MESSAGE(motion.can_reach(X_MIN_POS - past, mid_y), "past the X minimum is not reachable");
  TEST_ASSERT_FALSE_MESSAGE(motion.can_reach(X_MAX_POS + past, mid_y), "past the X maximum is not reachable");
  TEST_ASSERT_FALSE_MESSAGE(motion.can_reach(mid_x, Y_MIN_POS - past), "past the Y minimum is not reachable");
  TEST_ASSERT_FALSE_MESSAGE(motion.can_reach(mid_x, Y_MAX_POS + past), "past the Y maximum is not reachable");
}

/**
 * The slop is real, and it is small.
 *
 * Its whole purpose is that a coordinate computed to land on a limit is not refused because
 * the arithmetic came out a millionth of a millimetre the wrong side. Asserting both halves
 * says it is a rounding allowance and not a margin the machine will actually travel into.
 */
MARLIN_TEST(motion, the_reach_allows_a_rounding_error_but_not_a_margin) {
  const float mid_y = (Y_MIN_POS + Y_MAX_POS) * 0.5f;

  TEST_ASSERT_TRUE_MESSAGE(motion.can_reach(X_MAX_POS + fslop * 0.5f, mid_y),
    "a coordinate half a slop past the limit is a rounding error, and allowed");
  TEST_ASSERT_FALSE_MESSAGE(motion.can_reach(X_MAX_POS + 1.0f, mid_y),
    "a whole millimetre past the limit is not a rounding error");
}

#endif // !IS_KINEMATIC && HAS_X_AXIS && HAS_Y_AXIS

/**
 * The distance of a move is the length of the line it travels.
 *
 * It is what a feedrate is divided into to get a duration, so getting it wrong makes every
 * move the wrong speed — subtly, and in proportion to how diagonal it is. Pythagorean triples
 * make the expected answers exact rather than tolerances chosen to pass.
 */
MARLIN_TEST(motion, a_move_is_as_long_as_the_line_it_travels) {
  TERN_(HAS_ROTATIONAL_AXES, bool is_cartesian = true);
  auto length_of = [](const float dx, const float dy, const float dz) {
    xyze_pos_t diff = { 0 }; diff.x = dx; diff.y = dy; diff.z = dz;
    return motion.get_move_distance(diff OPTARG(HAS_ROTATIONAL_AXES, is_cartesian));
  };

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 4.0f, length_of(4.0f, 0.0f, 0.0f), "a move along X alone");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 5.0f, length_of(3.0f, 4.0f, 0.0f), "the hypotenuse of a 3-4 move");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 3.0f, length_of(1.0f, 2.0f, 2.0f), "a 1-2-2 move is three long");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f, length_of(0.0f, 0.0f, 0.0f), "a move that goes nowhere");

  // Sign cannot matter: a length is a length whichever way it is travelled.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, length_of(3.0f, 4.0f, 0.0f), length_of(-3.0f, -4.0f, 0.0f),
    "reversing a move should not change how long it is");
}

#if HAS_Z_AXIS
  /**
   * A move straight up is as long as it is tall.
   *
   * Taken separately in the source, ahead of the general case — so it is worth saying that
   * the shortcut agrees with the geometry it is a shortcut for.
   */
  MARLIN_TEST(motion, a_move_straight_up_is_as_long_as_it_is_tall) {
    TERN_(HAS_ROTATIONAL_AXES, bool is_cartesian = true);
    xyze_pos_t up = { 0 }; up.z = 7.0f;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 7.0f,
      motion.get_move_distance(up OPTARG(HAS_ROTATIONAL_AXES, is_cartesian)),
      "a purely vertical move is as long as its height");

    xyze_pos_t down = { 0 }; down.z = -7.0f;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 7.0f,
      motion.get_move_distance(down OPTARG(HAS_ROTATIONAL_AXES, is_cartesian)),
      "and downward is the same length as upward");
  }
#endif

/**
 * Syncing one axis from the steppers leaves the others where they were.
 *
 * The whole-machine form and the single-axis form are the same call with a different
 * argument, and the difference only shows in what is *not* changed — so the other axes are
 * deliberately set to something the steppers do not agree with before asking.
 */
MARLIN_TEST(motion, syncing_one_axis_from_the_steppers_leaves_the_others_alone) {
  const xyze_pos_t was = motion.position;

  motion.set_all_homed();
  xyze_pos_t made_up = motion.position;
  NUM_AXIS_CODE(made_up.x += 3.0f, made_up.y += 5.0f, made_up.z += 7.0f,
                made_up.i += 0.0f, made_up.j += 0.0f, made_up.k += 0.0f,
                made_up.u += 0.0f, made_up.v += 0.0f, made_up.w += 0.0f);
  motion.position = made_up;

  motion.set_current_from_steppers_for_axis(X_AXIS);

  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(made_up.y, motion.position.y,
      "syncing X should not have touched Y");
  #endif
  #if HAS_Z_AXIS
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(made_up.z, motion.position.z,
      "syncing X should not have touched Z");
  #endif

  // ...and the whole-machine form does touch them.
  motion.position = made_up;
  motion.set_current_from_steppers_for_axis(ALL_AXES_ENUM);
  #if HAS_Z_AXIS
    TEST_ASSERT_TRUE_MESSAGE(fabsf(motion.position.z - made_up.z) > 1e-4f,
      "syncing every axis should have replaced the made-up Z");
  #endif

  motion.position = was;
}

#if HAS_ENDSTOPS

/**
 * The second, slower approach to an endstop runs at a stated fraction of the first.
 *
 * Homing touches the switch twice: fast to find it, slowly to find it accurately. How much
 * slower is `HOMING_BUMP_DIVISOR`, and the relation between the two speeds is the whole
 * content of the setting — so that is what this asserts, rather than the number that comes
 * out of it.
 */
MARLIN_TEST(motion, the_homing_bump_feedrate_is_the_homing_feedrate_divided_down) {
  constexpr uint8_t divisor[] = HOMING_BUMP_DIVISOR;

  #if HAS_X_AXIS
    TEST_ASSERT_TRUE_MESSAGE(divisor[X_AXIS] >= 1, "a divisor below one would be a warning, not a speed");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f,
      motion.homing_feedrate(X_AXIS) / float(divisor[X_AXIS]),
      motion.get_homing_bump_feedrate(X_AXIS),
      "the X bump feedrate should be the X homing feedrate over the X divisor");
  #endif
  #if HAS_Y_AXIS
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f,
      motion.homing_feedrate(Y_AXIS) / float(divisor[Y_AXIS]),
      motion.get_homing_bump_feedrate(Y_AXIS),
      "and the Y bump feedrate the Y homing feedrate over the Y divisor");
  #endif

  // Slower, not merely different — the re-bump exists to be more accurate than the first.
  TEST_ASSERT_TRUE_MESSAGE(motion.get_homing_bump_feedrate(X_AXIS) < motion.homing_feedrate(X_AXIS),
    "the re-bump must be slower than the approach that found the switch");
}

#if HOMING_Z_WITH_PROBE
  /**
   * A machine that homes Z with its probe uses the probe's own slow speed.
   *
   * The bump divisor describes a limit switch. A probe has a speed of its own, chosen for the
   * same reason and stated separately, and Z homing is a probe — so the divisor does not
   * apply to it.
   */
  MARLIN_TEST(motion, homing_Z_with_a_probe_uses_the_probes_own_slow_speed) {
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, motion.z_probe_slow_mm_s,
      motion.get_homing_bump_feedrate(Z_AXIS),
      "Z homing is a probe, so its slow pass should run at the slow probing feedrate");
  }
#endif

#endif // HAS_ENDSTOPS

MARLIN_TEST(motion, steppers_can_be_enabled_and_disabled_per_axis) {
  stepper.enable_axis(X_AXIS);
  TEST_ASSERT_TRUE(stepper.axis_is_enabled(X_AXIS));

  stepper.disable_axis(X_AXIS);
  TEST_ASSERT_FALSE(stepper.axis_is_enabled(X_AXIS));
}

MARLIN_TEST(motion, all_steppers_can_be_enabled_and_disabled_together) {
  stepper.enable_all_steppers();
  TEST_ASSERT_TRUE(stepper.axis_is_enabled(X_AXIS));
  TEST_ASSERT_TRUE(stepper.axis_is_enabled(Y_AXIS));

  stepper.disable_all_steppers();
  TEST_ASSERT_FALSE(stepper.axis_is_enabled(X_AXIS));
  TEST_ASSERT_FALSE(stepper.axis_is_enabled(Y_AXIS));
}

// The stepper's idea of where it is can be set directly, which is what homing and
// G92.9 rely on.
MARLIN_TEST(motion, the_stepper_position_can_be_set_and_read) {
  stepper.set_axis_position(X_AXIS, 1234);
  TEST_ASSERT_EQUAL(1234, stepper.position(X_AXIS));

  stepper.set_axis_position(X_AXIS, -500);
  TEST_ASSERT_EQUAL(-500, stepper.position(X_AXIS));

  stepper.set_axis_position(X_AXIS, 0);
  TEST_ASSERT_EQUAL(0, stepper.position(X_AXIS));
}
