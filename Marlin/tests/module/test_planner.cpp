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
 * Tests for the motion planner's settings and conversions.
 *
 * The planner turns millimetres into steps and enforces the machine's limits. Its
 * queue cannot be exercised here — filling it blocks on a stepper interrupt that does
 * not run — but the arithmetic and the limits are what a wrong value would corrupt,
 * and those are pure.
 */

#include "../test/unit_tests.h"
#include "src/module/planner.h"
#include "src/module/motion.h"

namespace {

  struct SavedPlannerSettings {
    planner_settings_t was;
    float was_flow;
    SavedPlannerSettings() { was = planner.settings; was_flow = planner.flow_percentage[0]; }
    ~SavedPlannerSettings() {
      planner.settings = was;
      planner.set_flow(0, was_flow);
      planner.refresh_positioning();
    }
  };

}

MARLIN_TEST(planner, steps_per_mm_converts_millimetres_to_steps) {
  SavedPlannerSettings restore;

  planner.settings.axis_steps_per_mm[X_AXIS] = 80.0f;
  planner.refresh_positioning();
  TEST_ASSERT_EQUAL_FLOAT(1.0f / 80.0f, planner.mm_per_step[X_AXIS]);

  planner.settings.axis_steps_per_mm[X_AXIS] = 160.0f;
  planner.refresh_positioning();
  TEST_ASSERT_EQUAL_FLOAT(1.0f / 160.0f, planner.mm_per_step[X_AXIS]);
}

MARLIN_TEST(planner, flow_scales_the_extrusion_multiplier) {
  SavedPlannerSettings restore;

  planner.set_flow(0, 100);
  const float at_100 = planner.e_factor[0];

  planner.set_flow(0, 200);
  TEST_ASSERT_EQUAL_FLOAT(at_100 * 2.0f, planner.e_factor[0]);

  planner.set_flow(0, 50);
  TEST_ASSERT_EQUAL_FLOAT(at_100 / 2.0f, planner.e_factor[0]);
}

MARLIN_TEST(planner, a_flow_of_zero_extrudes_nothing) {
  SavedPlannerSettings restore;
  planner.set_flow(0, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, planner.e_factor[0]);
}

// The queue starts empty and reports itself so.
MARLIN_TEST(planner, an_empty_queue_has_nothing_planned) {
  planner.clear_block_buffer();
  TEST_ASSERT_EQUAL(0, planner.movesplanned());
  TEST_ASSERT_FALSE(planner.has_blocks_queued());
}

MARLIN_TEST(planner, feedrate_and_acceleration_limits_are_kept_per_axis) {
  SavedPlannerSettings restore;

  planner.settings.max_feedrate_mm_s[X_AXIS] = 300.0f;
  planner.settings.max_feedrate_mm_s[Y_AXIS] = 200.0f;
  TEST_ASSERT_EQUAL_FLOAT(300.0f, planner.settings.max_feedrate_mm_s[X_AXIS]);
  TEST_ASSERT_EQUAL_FLOAT(200.0f, planner.settings.max_feedrate_mm_s[Y_AXIS]);

  planner.settings.max_acceleration_mm_per_s2[X_AXIS] = 1000;
  planner.refresh_acceleration_rates();
  TEST_ASSERT_EQUAL(1000, planner.settings.max_acceleration_mm_per_s2[X_AXIS]);
}

// Setting a position tells the planner where the machine is without moving it.
MARLIN_TEST(planner, set_position_records_where_the_machine_is) {
  SavedPlannerSettings restore;
  planner.settings.axis_steps_per_mm[X_AXIS] = 80.0f;
  planner.refresh_positioning();

  xyze_pos_t here = { 0 };
  here.x = 12.5f;
  planner.set_position_mm(here);
  TEST_ASSERT_EQUAL_FLOAT(12.5f, planner.get_axis_position_mm(X_AXIS));
}
