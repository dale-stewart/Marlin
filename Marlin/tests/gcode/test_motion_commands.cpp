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
 * Tests for the movement commands.
 *
 * A move is planned rather than performed: the command works out where the tool should
 * end up and hands a segment to the planner, and the stepper interrupt walks it there
 * later. These tests assert on the destination and on what reached the planner, which
 * is everything the command itself decides.
 *
 * G28 is absent on purpose. Homing moves until an endstop triggers, and only the
 * stepper interrupt can trigger one, so in this build it never returns — verified by
 * probing it. Homing needs the NATIVE_SIM HAL; see docs/legacy-rescue-plan.md.
 */

#include "../test/unit_tests.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/MarlinCore.h"
#include <string.h>

namespace {

  /**
   * A stationary printer with a believable machine underneath it.
   *
   * axis_steps_per_mm defaults to zero in this build — nothing loads the configured
   * values without EEPROM — and a move of zero steps produces no planner block at all,
   * so anything asserting on what was planned would silently pass for the wrong reason.
   */
  /**
   * A printer that is switched on and not moving.
   *
   * The running state matters: every motion command begins with
   * `motion.gcode_motion_ignored()`, which is true while the firmware is not running,
   * and nothing calls setup() in a test build. Without this the moves below are
   * accepted, acknowledged and silently discarded — which looks exactly like a passing
   * test that asserts nothing.
   */
  struct Stationary {
    bool was_connected;
    xyze_pos_t was_position;
    MarlinState was_state;

    Stationary() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      was_state = marlin.state;
      marlin.setState(MF_RUNNING);
      was_position = motion.position;
      planner.clear_block_buffer();
    }
    ~Stationary() {
      planner.clear_block_buffer();
      motion.position = was_position;
      marlin.setState(was_state);
      MYSERIAL1.host_connected = was_connected;
    }
  };

  void host_sends(const char * const line) {
    static char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

}

MARLIN_TEST(motion_commands, G1_moves_to_an_absolute_position) {
  Stationary still;

  host_sends("G90");                       // absolute coordinates
  host_sends("G1 X10 Y20 F3000");
  TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.position.x);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, motion.position.y);

  // Asking for the same place again is still that place.
  host_sends("G1 X10 Y20");
  TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.position.x);
}

MARLIN_TEST(motion_commands, G91_makes_moves_relative) {
  Stationary still;

  host_sends("G90");
  host_sends("G1 X10 F3000");
  host_sends("G91");                       // relative coordinates
  host_sends("G1 X5");
  TEST_ASSERT_EQUAL_FLOAT(15.0f, motion.position.x);

  host_sends("G1 X-5");
  TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.position.x);

  host_sends("G90");
}

// An axis the command does not mention keeps its position.
MARLIN_TEST(motion_commands, an_unmentioned_axis_does_not_move) {
  Stationary still;

  host_sends("G90");
  host_sends("G1 X10 Y20 F3000");
  host_sends("G1 X30");
  TEST_ASSERT_EQUAL_FLOAT(30.0f, motion.position.x);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, motion.position.y);
}

MARLIN_TEST(motion_commands, F_sets_the_feedrate_for_later_moves) {
  Stationary still;

  host_sends("G90");
  host_sends("G1 X1 F1200");
  TEST_ASSERT_EQUAL_FLOAT(1200.0f / 60.0f, motion.feedrate_mm_s);

  // A move without F keeps the rate.
  host_sends("G1 X2");
  TEST_ASSERT_EQUAL_FLOAT(1200.0f / 60.0f, motion.feedrate_mm_s);
}

MARLIN_TEST(motion_commands, G0_and_G1_both_move) {
  Stationary still;

  host_sends("G90");
  host_sends("G0 X7 F3000");
  TEST_ASSERT_EQUAL_FLOAT(7.0f, motion.position.x);

  host_sends("G1 X8");
  TEST_ASSERT_EQUAL_FLOAT(8.0f, motion.position.x);
}

/**
 * Arcs (G2/G3) are absent.
 *
 * An arc is broken into many short segments, which fills the planner's block buffer;
 * buffer_line then waits for space that only the stepper interrupt can free, so the
 * command never returns. A single G1 fits in the buffer and is fine. Arcs need the
 * NATIVE_SIM HAL, whose stepper model drains the queue — see docs/legacy-rescue-plan.md.
 */

// G4 (dwell) is absent for the same reason as G28: it waits for the planner to drain
// before counting down, and nothing drains it here, so it never returns. Verified by
// probing. It needs the NATIVE_SIM HAL.
