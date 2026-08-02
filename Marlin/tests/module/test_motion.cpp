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
