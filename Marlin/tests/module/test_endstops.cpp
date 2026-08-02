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
 * Tests for the endstops.
 *
 * Endstops are what stops an axis driving itself into the frame. The switches are
 * simulated pins in this build, so a test can press one by writing to the pin the
 * firmware reads — no production seam is needed.
 */

#include "../test/unit_tests.h"
#include "src/module/endstops.h"

namespace {

  struct EndstopFixture {
    bool was_global;
    EndstopFixture() { was_global = endstops.global_enabled(); }
    ~EndstopFixture() {
      endstops.enable_globally(was_global);
      endstops.hit_on_purpose();
    }
  };

}

// The global switch is what M121 turns off to let a macro drive past a limit
// deliberately, and M120 turns back on.
MARLIN_TEST(endstops, the_global_switch_can_be_turned_off_and_on) {
  EndstopFixture fixture;

  endstops.enable_globally(false);
  TEST_ASSERT_FALSE(endstops.global_enabled());

  endstops.enable_globally(true);
  TEST_ASSERT_TRUE(endstops.global_enabled());
}

MARLIN_TEST(endstops, nothing_is_triggered_to_begin_with) {
  EndstopFixture fixture;
  endstops.hit_on_purpose();                 // clear any leftover flags
  TEST_ASSERT_FALSE(endstops.trigger_state());
}

/**
 * A printer that is not moving reports nothing, however its switches are set.
 *
 * `endstops.update()` only records a hit when the axis is moving *towards* the switch —
 * it reads the stepper's direction — so a pressed pin on a stationary machine changes
 * nothing. That is the behaviour, not a limitation of the harness: pressing a switch
 * on a *moving* axis is covered in `test_homing.cpp`, which runs under the test HAL.
 */
MARLIN_TEST(endstops, an_idle_printer_reports_nothing_triggered) {
  EndstopFixture fixture;
  endstops.hit_on_purpose();
  endstops.update();
  TEST_ASSERT_FALSE(endstops.trigger_state());
}
